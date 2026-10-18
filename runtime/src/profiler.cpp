// In-process sampling profiler (Apple platforms), for devices where
// Instruments cannot attach. Enabled with DIRT2_PROFILE=1 or a file named
// "profile" in the data folder (rt::start_profiler).
//
// A thread samples every other thread about once a millisecond: the program
// counter and a frame-pointer walk of its stack. Every 10 s it logs
//   - per thread: busy samples (a thread whose PC is in libsystem_kernel is
//     waiting, so it counts as idle) and its name;
//   - the top functions of the main image by self time (the innermost frame
//     in the main image) and by inclusive time (any frame, once per sample),
// as offsets from the image's unslid base, so `atos -o <binary> -l 0x100000000
// 0x1<offset>` names them (lifted functions are f_<guest address>).
#include "recomp/host.h"

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <pthread.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt {

namespace {

struct Range {
    uintptr_t lo = 0, hi = 0;
    bool contains(uintptr_t a) const { return a >= lo && a < hi; }
};

Range image_text(uint32_t index) {
    Range r;
    const auto* h = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(index));
    const intptr_t slide = _dyld_get_image_vmaddr_slide(index);
    const auto* lc = reinterpret_cast<const load_command*>(h + 1);
    for (uint32_t i = 0; i < h->ncmds; ++i) {
        if (lc->cmd == LC_SEGMENT_64) {
            const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
            if (std::strcmp(seg->segname, "__TEXT") == 0) {
                r.lo = seg->vmaddr + slide;
                r.hi = r.lo + seg->vmsize;
            }
        }
        lc = reinterpret_cast<const load_command*>(reinterpret_cast<const uint8_t*>(lc) + lc->cmdsize);
    }
    return r;
}

void run_profiler() {
    const Range main_text = image_text(0);
    const uintptr_t main_base = main_text.lo;  // unslid 0x100000000
    // libsystem_kernel: a PC there means the thread is blocked in a syscall.
    Range kernel;
    for (uint32_t i = 0; i < _dyld_image_count(); ++i)
        if (std::strstr(_dyld_get_image_name(i), "libsystem_kernel")) kernel = image_text(i);

    const mach_port_t self = mach_thread_self();
    std::unordered_map<uintptr_t, uint32_t> self_time, incl_time;
    std::map<std::string, std::pair<uint32_t, uint32_t>> threads;  // name -> busy, total
    uint32_t samples = 0;
    auto last = std::chrono::steady_clock::now();
    std::vector<uintptr_t> frames;
    std::unordered_set<uintptr_t> seen;

    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
        thread_act_array_t list;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) continue;
        for (mach_msg_type_number_t t = 0; t < count; ++t) {
            const thread_act_t th = list[t];
            if (th == self) {
                mach_port_deallocate(mach_task_self(), th);
                continue;
            }
            arm_thread_state64_t st;
            mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(th, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&st), &n) == KERN_SUCCESS) {
                const uintptr_t pc = arm_thread_state64_get_pc(st);
                char name[64] = "?";
                if (pthread_t p = pthread_from_mach_thread_np(th)) pthread_getname_np(p, name, sizeof name);
                if (!name[0]) std::snprintf(name, sizeof name, "thread %u", th);
                auto& tc = threads[name];
                ++tc.second;
                if (!kernel.contains(pc)) {
                    ++tc.first;
                    // Frames: PC, then the frame-pointer chain (bounded, sanity-checked).
                    frames.clear();
                    frames.push_back(pc);
                    frames.push_back(arm_thread_state64_get_lr(st));
                    uintptr_t fp = arm_thread_state64_get_fp(st);
                    for (int depth = 0; depth < 48 && fp && (fp & 7) == 0; ++depth) {
                        const uintptr_t* f = reinterpret_cast<const uintptr_t*>(fp);
                        const uintptr_t next = f[0], ret = f[1];
                        if (ret) frames.push_back(ret);
                        if (next <= fp || next - fp > (8u << 20)) break;
                        fp = next;
                    }
                    bool have_self = false;
                    seen.clear();
                    for (uintptr_t a : frames) {
                        if (!main_text.contains(a)) continue;
                        const uintptr_t off = a - main_base;
                        if (!have_self) {
                            ++self_time[off];
                            have_self = true;
                        }
                        if (seen.insert(off).second) ++incl_time[off];
                    }
                    if (!have_self) ++self_time[0];  // outside the main image (Metal, libsystem...)
                }
            }
            mach_port_deallocate(mach_task_self(), th);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(list), count * sizeof(thread_act_t));
        ++samples;

        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(10)) continue;
        last = now;
        log("prof: %u sample rounds", samples);
        std::vector<std::pair<std::string, std::pair<uint32_t, uint32_t>>> tv(threads.begin(), threads.end());
        std::sort(tv.begin(), tv.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
        for (size_t i = 0; i < tv.size() && i < 12; ++i)
            log("prof: thread %-24s busy %5.1f%%", tv[i].first.c_str(), 100.0 * tv[i].second.first / std::max(1u, tv[i].second.second));
        auto top = [](const std::unordered_map<uintptr_t, uint32_t>& m, const char* what) {
            std::vector<std::pair<uintptr_t, uint32_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            uint32_t total = 0;
            for (auto& [k, c] : v) total += c;
            for (size_t i = 0; i < v.size() && i < 40; ++i)
                log("prof: %s %5.2f%% 0x%llx", what, 100.0 * v[i].second / std::max(1u, total),
                    (unsigned long long)(v[i].first + 0x100000000ull));
        };
        top(self_time, "self");
        top(incl_time, "incl");
        self_time.clear();
        incl_time.clear();
        threads.clear();
        samples = 0;
    }
}

}  // namespace

void start_profiler() {
    static bool started = false;
    if (started) return;
    started = true;
    std::thread([] {
        pthread_setname_np("profiler");
        run_profiler();
    }).detach();
    log("prof: sampling profiler started");
}

}  // namespace rt

#else
namespace rt {
void start_profiler() {}
}  // namespace rt
#endif

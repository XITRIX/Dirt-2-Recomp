// The renderer's list of dynamic environment maps (a std::vector of entry
// pointers at owner+0x5840). 00CD70D0 walks it to render each "temp_envmap";
// 00D95690 appends (only caller 00D8F0E0, which builds an entry per car);
// 00D8A040 and 00D8A0E0 release every entry and free the buffer.
//
// A DX11 race on iPhone crashed in the walk reading an entry pointer of
// 0x00006563 from the list buffer: the buffer had been freed and reused while
// the walk held an iterator into it. The walk keeps begin/end iterators across
// virtual calls, so the list must not change under it:
//  - an append during a walk (from any thread) is queued and applied when the
//    walk ends, so the buffer is never reallocated under the iterator;
//  - a clear from another thread waits for the walk to end (bounded, so a
//    lock-order cycle with game locks degrades to the old behaviour, logged);
//  - a walk waits for appends and clears already in progress.
// Each case is logged, so a device log says which one happened.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "../shim.h"

namespace {

constexpr uint32_t kListOffset = 0x5840;  // {allocator, begin, end, capacity}
constexpr auto kClearWait = std::chrono::seconds(2);

std::mutex g_mu;
std::condition_variable g_cv;
uint32_t g_walk_list = 0;  // list being walked, or 0
uint32_t g_walker = 0;     // its thread
int g_busy = 0;            // appends and clears in progress outside a walk
std::vector<std::pair<uint32_t, uint32_t>> g_pending;  // (list, entry) appended during a walk
int g_logged = 0;

uint32_t entries(uint32_t list) { return (rd32(list + 8) - rd32(list + 4)) / 4; }

void note(const char* fmt, uint32_t list, uint32_t tid, const char* what) {
    if (g_logged++ < 40)
        rt::log(fmt, list, what, tid, g_walker, tid == g_walker ? "same thread" : "other thread", entries(list));
}

// thiscall (list, const entry**) on the guest stack built here; ret 4.
extern "C" void f_00D95690_orig(X86* c);
void append_now(X86* c, uint32_t list, uint32_t entry) {
    const uint32_t eax = c->eax, ecx = c->ecx, edx = c->edx, esp = c->esp;
    push32(c, entry);
    const uint32_t slot = c->esp;
    push32(c, slot);
    push32(c, rt::kReturnSentinel);
    c->ecx = list;
    f_00D95690_orig(c);
    c->eax = eax, c->ecx = ecx, c->edx = edx, c->esp = esp;
}

// Runs a change to the list (append or clear) unless a walk is on.
template <typename F>
void change(X86* c, uint32_t list, const char* what, F&& run) {
    const uint32_t tid = rt::current_thread_id();
    {
        std::unique_lock lk(g_mu);
        if (g_walk_list == list && g_walker != tid &&
            !g_cv.wait_for(lk, kClearWait, [&] { return g_walk_list != list; }))
            note("envmap list %08X: %s by thread %u still waiting on the walk on thread %u (%s), %u entries; going ahead",
                 list, tid, what);
        if (g_walk_list == list && g_walker == tid)
            note("envmap list %08X: %s by thread %u inside the walk on thread %u (%s), %u entries", list, tid, what);
        ++g_busy;
    }
    run();
    {
        std::lock_guard lk(g_mu);
        --g_busy;
    }
    g_cv.notify_all();
}

}  // namespace

// thiscall (renderer, arg), ret 4
extern "C" void f_00CD70D0_orig(X86* c);
extern "C" void f_00CD70D0(X86* c) {
    const uint32_t list = rd32(c->ecx + 0x6B4) + kListOffset, tid = rt::current_thread_id();
    {
        std::unique_lock lk(g_mu);
        if (g_walk_list && g_walker == tid) {  // nested walk on this thread: leave the state to the outer one
            lk.unlock();
            f_00CD70D0_orig(c);
            return;
        }
        if (!g_cv.wait_for(lk, kClearWait, [] { return !g_walk_list && !g_busy; }) && g_logged++ < 40)
            rt::log("envmap list %08X: walk on thread %u still waiting (walk on %08X, %d changes in progress); going ahead",
                    list, tid, g_walk_list, g_busy);
        g_walk_list = list, g_walker = tid;
    }
    f_00CD70D0_orig(c);
    std::vector<std::pair<uint32_t, uint32_t>> pending;
    {
        std::lock_guard lk(g_mu);
        g_walk_list = 0, g_walker = 0;
        pending.swap(g_pending);
    }
    g_cv.notify_all();
    for (const auto& [l, entry] : pending) change(c, l, "queued append", [&] { append_now(c, l, entry); });
}

// thiscall (list, const entry**), ret 4
extern "C" void f_00D95690(X86* c) {
    const uint32_t list = c->ecx, entry = rd32(rd32(c->esp + 4));
    {
        std::lock_guard lk(g_mu);
        if (g_walk_list == list) {
            const uint32_t tid = rt::current_thread_id();
            note("envmap list %08X: %s by thread %u during the walk on thread %u (%s), %u entries; queued", list, tid,
                 "append");
            g_pending.emplace_back(list, entry);
            c->esp += 8;
            return;
        }
    }
    change(c, list, "append", [&] { f_00D95690_orig(c); });
}

// thiscall (owner): release every entry and free the buffer
extern "C" void f_00D8A040_orig(X86* c);
extern "C" void f_00D8A040(X86* c) {
    change(c, c->ecx + kListOffset, "clear (00D8A040)", [&] { f_00D8A040_orig(c); });
}

extern "C" void f_00D8A0E0_orig(X86* c);
extern "C" void f_00D8A0E0(X86* c) {
    change(c, c->ecx + kListOffset, "clear (00D8A0E0)", [&] { f_00D8A0E0_orig(c); });
}

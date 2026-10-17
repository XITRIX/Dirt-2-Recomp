// Guest threads: each host thread owns an X86 context, a guest stack, a TEB
// and a copy of the image's static TLS block.
#include <pthread.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "recomp/host.h"

namespace rt {

struct Thread {
    X86 cpu{};
    uint32_t id = 0;
    uint32_t teb = 0, stack_base = 0, stack_size = 0;
    uint32_t entry = 0, param = 0;
    pthread_t host{};
};

namespace {

constexpr uint32_t kTebSize = 0x1000;
constexpr uint32_t kHostStack = 64u << 20;  // guest call depth maps onto the host stack

thread_local Thread* t_cur = nullptr;
std::atomic<uint32_t> g_next_id{0x100};
uint32_t g_peb = 0;
std::mutex g_mu;
std::unordered_map<uint32_t, Thread*> g_threads;

void init_peb() {
    if (g_peb) return;
    g_peb = vm_alloc(0, kPage, true);
    wr32(g_peb + 0x08, recomp_image_info.image_base);  // ImageBaseAddress
}

Thread* make_thread(uint32_t stack_size) {
    init_peb();
    auto* t = new Thread;
    t->id = g_next_id += 4;
    t->stack_size = stack_size ? (stack_size + 0xFFFF) & ~0xFFFFu : 1u << 20;
    t->stack_base = vm_alloc(0, t->stack_size, true);
    t->teb = vm_alloc(0, kTebSize, true);
    if (!t->stack_base || !t->teb) fatal("cannot allocate thread stack/TEB");
    const uint32_t top = t->stack_base + t->stack_size;
    wr32(t->teb + 0x00, 0xFFFFFFFF);    // SEH chain end
    wr32(t->teb + 0x04, top);           // StackBase
    wr32(t->teb + 0x08, t->stack_base); // StackLimit
    wr32(t->teb + 0x18, t->teb);        // Self
    wr32(t->teb + 0x20, 0x10);          // ClientId.UniqueProcess
    wr32(t->teb + 0x24, t->id);         // ClientId.UniqueThread
    wr32(t->teb + 0x30, g_peb);

    // Static TLS: ThreadLocalStoragePointer[index] -> copy of the template.
    const RecompImageInfo& ii = recomp_image_info;
    if (ii.tls_end > ii.tls_start || ii.tls_zero_fill) {
        const uint32_t tsize = ii.tls_end - ii.tls_start + ii.tls_zero_fill;
        const uint32_t block = heap_alloc(tsize + 16, true);
        std::memcpy(host_ptr(block), host_ptr(ii.tls_start), ii.tls_end - ii.tls_start);
        const uint32_t vec = heap_alloc(64 * 4, true);
        wr32(vec, block);
        wr32(t->teb + 0x2C, vec);
        if (ii.tls_index) wr32(ii.tls_index, 0);
    }

    X86& c = t->cpu;
    c.esp = (top - 64) & ~15u;
    c.fs_base = t->teb;
    c.fcw = 0x027F;   // Windows default: 53-bit precision, exceptions masked
    c.ftag = 0xFFFF;
    c.mxcsr = 0x1F80;
    c.host = t;
    std::lock_guard lk(g_mu);
    g_threads[t->id] = t;
    return t;
}

void* thread_main(void* arg) {
    auto* t = static_cast<Thread*>(arg);
    t_cur = t;
    const uint32_t r = invoke(&t->cpu, t->entry, {t->param}, true);
    log("thread %u exited with %u", t->id, r);
    return nullptr;
}

}  // namespace

X86* current_cpu() { return t_cur ? &t_cur->cpu : nullptr; }
Thread* current_thread() { return t_cur; }
uint32_t current_thread_id() { return t_cur ? t_cur->id : 0; }

X86* create_main_thread() {
    Thread* t = make_thread(2u << 20);
    t_cur = t;
    return &t->cpu;
}

uint32_t start_thread(uint32_t entry, uint32_t param, uint32_t stack_size, bool suspended, uint32_t* handle_out) {
    Thread* t = make_thread(stack_size);
    t->entry = entry;
    t->param = param;
    (void)suspended;  // TODO: honour CREATE_SUSPENDED via a start gate
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStack);
    if (pthread_create(&t->host, &attr, thread_main, t) != 0) fatal("pthread_create failed");
    pthread_attr_destroy(&attr);
    if (handle_out) *handle_out = t->id;
    return t->id;
}

}  // namespace rt

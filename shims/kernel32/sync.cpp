// kernel32: waitable objects, threads, critical sections, interlocked, TLS.
//
// Waitable state is guarded by one mutex (held only for a few instructions),
// but each waiting thread sleeps on its own condition variable registered
// with the objects it waits for, so a signal wakes only that object's
// waiters. The game runs ~40 threads; a shared notify_all turned every
// SetEvent into a lock convoy that stalled loading.
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "../shim.h"

using namespace shim;
using Clock = std::chrono::steady_clock;

namespace {

std::mutex g_wmu;

struct Waiter {
    std::condition_variable cv;
};

struct Waitable : KObject {
    using KObject::KObject;
    std::vector<Waiter*> waiters;  // guarded by g_wmu
    void wake() {
        for (Waiter* w : waiters) w->cv.notify_one();
    }
    virtual bool signaled(uint32_t tid) const = 0;
    virtual void acquire(uint32_t tid) {}
    virtual Clock::time_point deadline() const { return Clock::time_point::max(); }
};

struct Event : Waitable {
    Event(bool m, bool s) : Waitable(Kind::Event), manual(m), set(s) {}
    bool manual, set;
    bool signaled(uint32_t) const override { return set; }
    void acquire(uint32_t) override {
        if (!manual) set = false;
    }
};

struct Mutex : Waitable {
    Mutex() : Waitable(Kind::Mutex) {}
    uint32_t owner = 0, count = 0;
    bool signaled(uint32_t tid) const override { return owner == 0 || owner == tid; }
    void acquire(uint32_t tid) override {
        owner = tid;
        ++count;
    }
};

struct Semaphore : Waitable {
    Semaphore(int32_t c, int32_t m) : Waitable(Kind::Semaphore), count(c), max(m) {}
    int32_t count, max;
    bool signaled(uint32_t) const override { return count > 0; }
    void acquire(uint32_t) override { --count; }
};

struct ThreadObj : Waitable {
    ThreadObj() : Waitable(Kind::Thread) {}
    uint32_t id = 0, exit_code = 259;  // STILL_ACTIVE
    bool done = false;
    bool signaled(uint32_t) const override { return done; }
};

struct Timer : Waitable {
    explicit Timer(bool m) : Waitable(Kind::Timer), manual(m) {}
    bool manual;
    bool active = false;
    Clock::time_point due;
    uint32_t period_ms = 0;
    bool signaled(uint32_t) const override { return active && Clock::now() >= due; }
    void acquire(uint32_t) override {
        if (manual) return;
        if (period_ms) due += std::chrono::milliseconds(period_ms);
        else active = false;
    }
    Clock::time_point deadline() const override { return active ? due : Clock::time_point::max(); }
};

constexpr uint32_t WAIT_TIMEOUT = 0x102, WAIT_FAILED = 0xFFFFFFFF, INFINITE = 0xFFFFFFFF;

uint32_t wait_handles(const std::vector<uint32_t>& hs, bool all, uint32_t timeout_ms) {
    std::vector<std::shared_ptr<Waitable>> objs;
    for (uint32_t h : hs) {
        auto w = handle_as<Waitable>(h);
        if (!w) {
            set_last_error(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        objs.push_back(std::move(w));
    }
    const uint32_t tid = rt::current_thread_id();
    const auto end = timeout_ms == INFINITE ? Clock::time_point::max() : Clock::now() + std::chrono::milliseconds(timeout_ms);
    Waiter self;
    std::unique_lock lk(g_wmu);
    bool registered = false;
    struct Unregister {
        std::vector<std::shared_ptr<Waitable>>& objs;
        Waiter* w;
        bool& on;
        ~Unregister() {
            if (!on) return;
            for (auto& o : objs) std::erase(o->waiters, w);
        }
    } unregister{objs, &self, registered};
    for (;;) {
        if (all) {
            bool ok = true;
            for (auto& o : objs) ok = ok && o->signaled(tid);
            if (ok) {
                for (auto& o : objs) o->acquire(tid);
                return 0;
            }
        } else {
            for (size_t i = 0; i < objs.size(); ++i)
                if (objs[i]->signaled(tid)) {
                    objs[i]->acquire(tid);
                    return uint32_t(i);
                }
        }
        auto wake = end;
        for (auto& o : objs) wake = std::min(wake, o->deadline());
        if (timeout_ms == 0 || Clock::now() >= end) return WAIT_TIMEOUT;
        if (!registered) {
            for (auto& o : objs) o->waiters.push_back(&self);
            registered = true;
        }
        if (wake == Clock::time_point::max()) self.cv.wait(lk);
        else self.cv.wait_until(lk, wake);
    }
}

template <typename T, typename F>
uint32_t modify(uint32_t h, F&& f) {
    auto o = handle_as<T>(h);
    if (!o) {
        set_last_error(ERROR_INVALID_HANDLE);
        return 0;
    }
    std::lock_guard lk(g_wmu);
    const uint32_t r = f(*o);
    o->wake();
    return r;
}

// ---- critical sections: host mutex index stored in LockSemaphore (+16) ----
// Lookup is lock-free (fixed table of atomic pointers); only creation and
// deletion lock. DeleteCriticalSection recycles the slot (the DX11 renderer
// creates and deletes them continually while streaming: never recycling ran
// the table out mid-race). Host mutexes are never freed, so a lookup racing
// a delete never touches freed memory; a recycled slot reuses its mutex.
struct HostCS {
    std::recursive_mutex m;
};
constexpr uint32_t kMaxCS = 1u << 16;
std::mutex g_cs_mu;
std::atomic<HostCS*> g_cs[kMaxCS];
std::atomic<uint32_t> g_cs_count{0};
uint32_t g_cs_owner[kMaxCS];         // guest CRITICAL_SECTION address per slot (under g_cs_mu)
std::vector<uint32_t> g_cs_free;     // recycled slots (1-based)
std::atomic<uint32_t> g_cs_inits{0}, g_cs_deletes{0};

// A new slot for the critical section at p (g_cs_mu held).
uint32_t cs_new_slot(uint32_t p) {
    uint32_t slot;
    if (!g_cs_free.empty()) {
        slot = g_cs_free.back();
        g_cs_free.pop_back();
    } else {
        const uint32_t n = g_cs_count.load();
        if (n >= kMaxCS) rt::fatal("too many critical sections (%u live)", n);
        g_cs[n].store(new HostCS, std::memory_order_release);
        g_cs_count.store(n + 1, std::memory_order_release);
        slot = n + 1;
        if (slot % 4096 == 0)
            rt::log("kernel32: %u critical sections live (%u created, %u deleted)", slot, g_cs_inits.load(),
                    g_cs_deletes.load());
    }
    g_cs_owner[slot - 1] = p;
    __atomic_store_n(reinterpret_cast<uint32_t*>(rt::host_ptr(p + 16)), slot, __ATOMIC_RELEASE);
    return slot;
}

HostCS& cs_for(uint32_t p) {
    uint32_t slot = __atomic_load_n(reinterpret_cast<uint32_t*>(rt::host_ptr(p + 16)), __ATOMIC_ACQUIRE);
    if (slot && slot <= g_cs_count.load(std::memory_order_acquire))
        if (HostCS* cs = g_cs[slot - 1].load(std::memory_order_acquire)) return *cs;
    std::lock_guard lk(g_cs_mu);
    slot = rd32(p + 16);
    if (slot && slot <= g_cs_count.load()) return *g_cs[slot - 1].load();
    return *g_cs[cs_new_slot(p) - 1].load();
}

// InitializeCriticalSection*: zero the struct, keep this address's slot if it
// already has one (initialised twice without a delete), else take a new one.
void cs_init(uint32_t p) {
    std::lock_guard lk(g_cs_mu);
    ++g_cs_inits;
    const uint32_t old = rd32(p + 16);
    std::memset(rt::host_ptr(p), 0, 24);
    wr32(p + 4, 0xFFFFFFFF);  // LockCount
    if (old && old <= g_cs_count.load() && g_cs_owner[old - 1] == p)
        __atomic_store_n(reinterpret_cast<uint32_t*>(rt::host_ptr(p + 16)), old, __ATOMIC_RELEASE);
    else
        cs_new_slot(p);
}

void cs_delete(uint32_t p) {
    std::lock_guard lk(g_cs_mu);
    ++g_cs_deletes;
    const uint32_t slot = rd32(p + 16);
    if (!slot || slot > g_cs_count.load() || g_cs_owner[slot - 1] != p) return;
    g_cs_owner[slot - 1] = 0;
    g_cs_free.push_back(slot);
    wr32(p + 16, 0);
}

// ---- TLS slots: TEB + 0xE10 ----
std::mutex g_tls_mu;
uint64_t g_tls_used = 0;

}  // namespace

// ---- waits ----
bool log_waits() {
    static const bool on = std::getenv("DIRT2_LOG_WAITS") != nullptr;
    return on;
}

STDCALL(kernel32, WaitForSingleObject, 2) {
    const auto t0 = Clock::now();
    const uint32_t r = wait_handles({a[0]}, false, a[1]);
    if (log_waits()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (ms >= 50)
            rt::log("t%u WaitForSingleObject(%X, %d) = %X after %lld ms [from %08X]", rt::current_thread_id(), a[0],
                    int32_t(a[1]), r, (long long)ms, rd32(c->esp));
    }
    return r;
}
STDCALL(kernel32, WaitForMultipleObjects, 4) {
    std::vector<uint32_t> hs;
    for (uint32_t i = 0; i < a[0] && i < 64; ++i) hs.push_back(rd32(a[1] + 4 * i));
    const auto t0 = Clock::now();
    const uint32_t r = wait_handles(hs, a[2] != 0, a[3]);
    if (log_waits()) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (ms >= 50)
            rt::log("t%u WaitForMultipleObjects(%u, all=%u, %d) = %X after %lld ms [from %08X]", rt::current_thread_id(),
                    a[0], a[2], int32_t(a[3]), r, (long long)ms, rd32(c->esp));
    }
    return r;
}
STDCALL(kernel32, Sleep, 1) {
    if (a[0] == 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::milliseconds(a[0]));
    if (log_waits() && a[0] >= 50) rt::log("t%u Sleep(%u) [from %08X]", rt::current_thread_id(), a[0], rd32(c->esp));
    return 0;
}

// ---- events, mutexes, semaphores, timers ----
STDCALL(kernel32, CreateEventA, 4) { return handle_new(std::make_shared<Event>(a[1] != 0, a[2] != 0)); }
bool shim::set_event(uint32_t h) { return modify<Event>(h, [](Event& e) { e.set = true; return 1u; }) != 0; }
STDCALL(kernel32, SetEvent, 1) { return set_event(a[0]) ? 1 : 0; }
STDCALL(kernel32, ResetEvent, 1) { return modify<Event>(a[0], [](Event& e) { e.set = false; return 1u; }); }
STDCALL(kernel32, CreateMutexA, 3) {
    auto m = std::make_shared<Mutex>();
    if (a[1]) {
        m->owner = rt::current_thread_id();
        m->count = 1;
    }
    return handle_new(m);
}
STDCALL(kernel32, ReleaseMutex, 1) {
    const uint32_t tid = rt::current_thread_id();
    return modify<Mutex>(a[0], [tid](Mutex& m) {
        if (m.owner != tid) return 0u;
        if (--m.count == 0) m.owner = 0;
        return 1u;
    });
}
STDCALL(kernel32, CreateSemaphoreA, 4) { return handle_new(std::make_shared<Semaphore>(int32_t(a[1]), int32_t(a[2]))); }
STDCALL(kernel32, ReleaseSemaphore, 3) {
    const uint32_t n = a[1], prev_out = a[2];
    return modify<Semaphore>(a[0], [n, prev_out](Semaphore& s) {
        if (s.count + int32_t(n) > s.max) return 0u;
        if (prev_out) wr32(prev_out, uint32_t(s.count));
        s.count += int32_t(n);
        return 1u;
    });
}
STDCALL(kernel32, CreateWaitableTimerA, 3) { return handle_new(std::make_shared<Timer>(a[1] != 0)); }
STDCALL(kernel32, SetWaitableTimer, 6) {
    const int64_t due = int64_t(rd64(a[1]));  // 100 ns units; negative = relative
    const uint32_t period = a[2];
    return modify<Timer>(a[0], [due, period](Timer& t) {
        const auto rel = due < 0 ? std::chrono::nanoseconds(-due * 100) : std::chrono::nanoseconds(0);
        t.due = Clock::now() + rel;
        t.period_ms = period;
        t.active = true;
        return 1u;
    });
}
STDCALL(kernel32, CancelWaitableTimer, 1) { return modify<Timer>(a[0], [](Timer& t) { t.active = false; return 1u; }); }

// ---- threads ----
STDCALL(kernel32, CreateThread, 6) {
    auto t = std::make_shared<ThreadObj>();
    const uint32_t h = handle_new(t);
    std::weak_ptr<ThreadObj> weak = t;
    {
        std::lock_guard lk(g_wmu);  // keep on_exit from racing with id assignment
        t->id = rt::start_thread(a[2], a[3], a[1], (a[4] & 4) != 0, [weak](uint32_t code) {
            if (auto o = weak.lock()) {
                std::lock_guard lk2(g_wmu);
                o->done = true;
                o->exit_code = code;
                o->wake();
            }
        });
    }
    if (a[5]) wr32(a[5], t->id);
    rt::log("CreateThread start=0x%08X param=0x%08X -> tid %u", a[2], a[3], t->id);
    return h;
}
STDCALL(kernel32, ResumeThread, 1) {
    auto t = handle_as<ThreadObj>(a[0]);
    if (!t) return 0xFFFFFFFF;
    return uint32_t(rt::resume_thread(t->id));
}
STDCALL(kernel32, SuspendThread, 1) {
    rt::log("SuspendThread(0x%X) ignored", a[0]);
    return 0;
}
STDCALL(kernel32, GetExitCodeThread, 2) {
    auto t = handle_as<ThreadObj>(a[0]);
    if (!t) return 0;
    std::lock_guard lk(g_wmu);
    wr32(a[1], t->exit_code);
    return 1;
}
STDCALL(kernel32, ExitThread, 1) { rt::exit_current_thread(a[0]); }
STDCALL(kernel32, GetCurrentThreadId, 0) { return rt::current_thread_id(); }
STDCALL(kernel32, GetCurrentThread, 0) { return kCurrentThread; }

// ---- critical sections ----
STDCALL(kernel32, InitializeCriticalSection, 1) {
    if (std::getenv("DIRT2_LOG_CS")) rt::log("InitializeCriticalSection(%08X) from %08X", a[0], rd32(c->esp));
    cs_init(a[0]);
    return 0;
}
STDCALL(kernel32, DeleteCriticalSection, 1) {
    cs_delete(a[0]);
    return 0;
}
STDCALL(kernel32, EnterCriticalSection, 1) {
    cs_for(a[0]).m.lock();
    wr32(a[0] + 8, rd32(a[0] + 8) + 1);
    wr32(a[0] + 12, rt::current_thread_id());
    return 0;
}
STDCALL(kernel32, TryEnterCriticalSection, 1) {
    if (!cs_for(a[0]).m.try_lock()) return 0;
    wr32(a[0] + 8, rd32(a[0] + 8) + 1);
    wr32(a[0] + 12, rt::current_thread_id());
    return 1;
}
STDCALL(kernel32, LeaveCriticalSection, 1) {
    const uint32_t n = rd32(a[0] + 8) - 1;
    wr32(a[0] + 8, n);
    if (!n) wr32(a[0] + 12, 0);
    cs_for(a[0]).m.unlock();
    return 0;
}

// ---- interlocked (guest memory is host memory) ----
STDCALL(kernel32, InterlockedIncrement, 1) {
    return __atomic_add_fetch(reinterpret_cast<uint32_t*>(rt::host_ptr(a[0])), 1, __ATOMIC_SEQ_CST);
}
STDCALL(kernel32, InterlockedDecrement, 1) {
    return __atomic_sub_fetch(reinterpret_cast<uint32_t*>(rt::host_ptr(a[0])), 1, __ATOMIC_SEQ_CST);
}
STDCALL(kernel32, InterlockedExchange, 2) {
    return __atomic_exchange_n(reinterpret_cast<uint32_t*>(rt::host_ptr(a[0])), a[1], __ATOMIC_SEQ_CST);
}
STDCALL(kernel32, InterlockedExchangeAdd, 2) {
    return __atomic_fetch_add(reinterpret_cast<uint32_t*>(rt::host_ptr(a[0])), a[1], __ATOMIC_SEQ_CST);
}
STDCALL(kernel32, InterlockedCompareExchange, 3) {
    uint32_t expected = a[2];
    __atomic_compare_exchange_n(reinterpret_cast<uint32_t*>(rt::host_ptr(a[0])), &expected, a[1], false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return expected;
}

// ---- TLS ----
STDCALL(kernel32, TlsAlloc, 0) {
    std::lock_guard lk(g_tls_mu);
    for (uint32_t i = 0; i < 64; ++i)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            return i;
        }
    return 0xFFFFFFFF;
}
STDCALL(kernel32, TlsFree, 1) {
    std::lock_guard lk(g_tls_mu);
    g_tls_used &= ~(1ull << (a[0] & 63));
    return 1;
}
STDCALL(kernel32, TlsGetValue, 1) {
    if (a[0] >= 64) return 0;
    set_last_error(0);
    return rd32(rt::current_teb() + 0xE10 + 4 * a[0]);
}
STDCALL(kernel32, TlsSetValue, 2) {
    if (a[0] >= 64) return 0;
    wr32(rt::current_teb() + 0xE10 + 4 * a[0], a[1]);
    return 1;
}

// ---- handles ----
STDCALL(kernel32, CloseHandle, 1) {
    if (a[0] == kCurrentProcess || a[0] == kCurrentThread) return 1;
    return handle_close(a[0]) ? 1 : 0;
}
STDCALL(kernel32, DuplicateHandle, 7) {
    uint32_t h = a[1];
    if (h == kCurrentThread || h == kCurrentProcess) {
        // Pseudo handles duplicate to a real handle; a placeholder event is enough for the game.
        h = handle_new(std::make_shared<Event>(true, false));
    } else {
        h = handle_dup(h);
    }
    if (a[3]) wr32(a[3], h);
    return h ? 1 : 0;
}

// ---- APIs the CRT resolves dynamically (not imported, found via GetProcAddress) ----
STDCALL(kernel32, EncodePointer, 1) { return a[0]; }
STDCALL(kernel32, DecodePointer, 1) { return a[0]; }
STDCALL(kernel32, InitializeCriticalSectionAndSpinCount, 2) {
    cs_init(a[0]);
    return 1;
}
STDCALL(kernel32, FlsAlloc, 1) {
    std::lock_guard lk(g_tls_mu);
    for (uint32_t i = 0; i < 64; ++i)
        if (!(g_tls_used & (1ull << i))) {
            g_tls_used |= 1ull << i;
            return i;
        }
    return 0xFFFFFFFF;
}
STDCALL(kernel32, FlsFree, 1) {
    std::lock_guard lk(g_tls_mu);
    g_tls_used &= ~(1ull << (a[0] & 63));
    return 1;
}
STDCALL(kernel32, FlsGetValue, 1) { return a[0] < 64 ? rd32(rt::current_teb() + 0xE10 + 4 * a[0]) : 0; }
STDCALL(kernel32, FlsSetValue, 2) {
    if (a[0] >= 64) return 0;
    wr32(rt::current_teb() + 0xE10 + 4 * a[0], a[1]);
    return 1;
}

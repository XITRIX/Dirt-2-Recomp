// COM bridge core: the game's 32-bit COM calls -> a host COM implementation.
//
// Shared by the D3D9 bridge (DXVK) and the D3D11/DXGI bridge (DXMT). Each
// includes this header once, inside its own anonymous namespace and after its
// own COM headers, since DXVK's and DXMT's headers each declare IUnknown and
// cannot share a translation unit. So every bridge gets its own copy of the
// proxy tables, which is fine: a proxy is a guest address and only ever goes
// back to the bridge that made it.
//
// Every host object handed to the game is a small guest-memory proxy (vtable,
// magic) whose vtable entries are dispatch thunks into the bridge. A method's
// arguments are converted from guest stack slots by type (Arg<>):
//   - integers, enums and floats pass through;
//   - pointers to structs with the same layout in the 32-bit guest and the
//     64-bit host are used in place (guest memory is host memory);
//   - interfaces map proxy <-> host object; out-interfaces are wrapped;
//   - (REFIID, void**) outs are wrapped by the IID;
//   - interface arrays (ARRAY_PARAMS) are converted element by element;
//   - structs whose layout differs (LAYOUT_DIFFERS: pointers, SIZE_T, HANDLE)
//     need a hand-written OVERRIDE; without one the method is a logging stub.
//
// The including bridge defines, before this header:
//   constexpr const char* kBridge;          log prefix ("d3d9")
//   constexpr uint32_t kProxyMagic;         second dword of each proxy
//   constexpr HRESULT kInvalidCall;         result when `this` is not a proxy
//   bool is_leaf_name(const char* iface);   distinct-object interfaces (see wrap_with)
// and declares, for use here:
//   template <typename I> uint32_t wrap(I* p);         most-derived wrapping
//   uint32_t wrap_by_iid(const GUID& iid, void* obj);
//   void on_release(uint32_t proxy);                  drop per-proxy state
// It then specialises custom_arg<T> for types with its own Arg<>, and includes
// its generated vtables after its OVERRIDEs.
// The standard headers below must already be included above that namespace:
// included for the first time inside it, they would land in the wrong scope.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Proxies
// ---------------------------------------------------------------------------

struct VTableDesc {
    const char* name;
    const recomp_fn* fns;
    uint32_t count;
};
template <typename I>
const VTableDesc& vtable_desc();

struct Entry {
    IUnknown* host;
    const VTableDesc* desc;
};

std::recursive_mutex g_mu;
std::unordered_map<uint32_t, Entry> g_by_guest;
std::unordered_map<IUnknown*, uint32_t> g_by_host;
std::unordered_map<const VTableDesc*, uint32_t> g_vtables;

uint32_t guest_vtable(const VTableDesc& d) {
    auto it = g_vtables.find(&d);
    if (it != g_vtables.end()) return it->second;
    const uint32_t vt = rt::heap_alloc(d.count * 4);
    for (uint32_t i = 0; i < d.count; ++i) wr32(vt + 4 * i, rt::alloc_thunk(d.fns[i]));
    g_vtables[&d] = vt;
    return vt;
}

uint32_t wrap_with(IUnknown* host, const VTableDesc& d) {
    if (!host) return 0;
    std::lock_guard lk(g_mu);
    if (auto it = g_by_host.find(host); it != g_by_host.end()) {
        auto old = g_by_guest.find(it->second);
        // A child object (a D3D9 surface of a texture or swap chain) can die with
        // its container without the game's Release reaching zero, so its entry
        // outlives it, and the host then hands out the same address for a new
        // object. Returning the dead object's proxy for it made the game call
        // GetContainer where it meant Lock and memcpy to the lock flags (0x2800).
        // A host pointer that comes back as a different distinct-object
        // interface is such a reuse: retire the old proxy.
        if (old == g_by_guest.end() || old->second.desc == &d || !is_leaf_name(old->second.desc->name) ||
            !is_leaf_name(d.name))
            return it->second;
        static std::atomic<int> logged{0};
        if (logged++ < 20)
            rt::log("%s: host %p reused: %s proxy 0x%08X retired for a new %s", kBridge, static_cast<void*>(host),
                    old->second.desc->name, it->second, d.name);
        g_by_guest.erase(old);
        g_by_host.erase(it);
    }
    const uint32_t p = rt::heap_alloc(16, true);
    wr32(p, guest_vtable(d));
    wr32(p + 4, kProxyMagic);
    g_by_guest[p] = Entry{host, &d};
    g_by_host[host] = p;
    return p;
}

IUnknown* to_host(uint32_t guest) {
    if (!guest) return nullptr;
    std::lock_guard lk(g_mu);
    auto it = g_by_guest.find(guest);
    if (it == g_by_guest.end()) {
        rt::log("%s: unknown proxy 0x%08X", kBridge, guest);
        return nullptr;
    }
    return it->second.host;
}

void forget(uint32_t guest) {
    std::lock_guard lk(g_mu);
    auto it = g_by_guest.find(guest);
    if (it == g_by_guest.end()) return;
    g_by_host.erase(it->second.host);
    g_by_guest.erase(it);
    // The proxy memory is left in place: the game may still compare the pointer.
}

// ---------------------------------------------------------------------------
// Argument conversion, one 32-bit guest stack slot per argument
// ---------------------------------------------------------------------------

template <typename T>
constexpr bool is_interface_v = std::is_base_of_v<IUnknown, std::remove_cv_t<T>>;

// Types the bridge converts with its own Arg<> specialisation.
template <typename T>
struct custom_arg : std::false_type {};
// Structs laid out differently in the guest (generated LAYOUT_DIFFERS).
template <typename T>
struct layout_differs : std::false_type {};
#define LAYOUT_DIFFERS(T) \
    template <>           \
    struct layout_differs<T> : std::true_type {};

template <typename T, typename = void>
struct Arg;

template <typename T>
struct Arg<T, std::enable_if_t<std::is_integral_v<T> || std::is_enum_v<T>>> {
    static constexpr bool unsafe = false;
    uint32_t s;
    T get() const { return T(s); }
    void done() {}
};

template <>
struct Arg<float> {
    static constexpr bool unsafe = false;
    uint32_t s;
    float get() const { float f; std::memcpy(&f, &s, 4); return f; }
    void done() {}
};

// Interface in.
template <typename I>
struct Arg<I*, std::enable_if_t<is_interface_v<I>>> {
    static constexpr bool unsafe = false;
    uint32_t s;
    I* get() const { return static_cast<I*>(to_host(s)); }
    void done() {}
};

// Interface out: write the (possibly new) proxy back.
template <typename I>
struct Arg<I**, std::enable_if_t<is_interface_v<I>>> {
    static constexpr bool unsafe = false;
    uint32_t s;
    I* v = nullptr;
    I** get() { return s ? &v : nullptr; }
    void done() {
        if (s) wr32(s, wrap(v));
    }
};

// void** outs without an IID before them (D3D9's HANDLE* pSharedHandle): NULL.
template <>
struct Arg<void**> {
    static constexpr bool unsafe = false;
    uint32_t s;
    void* v = nullptr;
    void** get() { return s ? &v : nullptr; }
    void done() {
        if (s) wr32(s, 0);
    }
};

// REFIID / REFGUID.
template <>
struct Arg<const GUID&> {
    static constexpr bool unsafe = false;
    uint32_t s;
    const GUID& get() const { return *reinterpret_cast<const GUID*>(rt::host_ptr(s)); }
    void done() {}
};

// Interface arrays the generator found no count for, and structs by value:
// no generic conversion (the method becomes a logging stub unless overridden).
template <typename I>
struct Arg<I* const*, std::enable_if_t<is_interface_v<I>>> {
    static constexpr bool unsafe = true;
    uint32_t s;
    I* const* get() const { return nullptr; }
    void done() {}
};
template <typename T>
struct Arg<T, std::enable_if_t<std::is_class_v<T>>> {
    static constexpr bool unsafe = true;
    uint32_t s;
    T get() const { return T{}; }
    void done() {}
};

// Pointers to data laid out alike: guest memory used in place.
template <typename T>
struct Arg<T*, std::enable_if_t<!is_interface_v<T> && !std::is_pointer_v<T> && !custom_arg<std::remove_cv_t<T>>::value>> {
    static constexpr bool unsafe = layout_differs<std::remove_cv_t<T>>::value;
    uint32_t s;
    T* get() const { return s ? reinterpret_cast<T*>(rt::host_ptr(s)) : nullptr; }
    void done() {}
};

// (REFIID riid, void** ppv): the out object is wrapped as the IID's interface.
struct IidOutArg {
    static constexpr bool unsafe = false;
    uint32_t s;
    const GUID* iid = nullptr;
    void* v = nullptr;
    void** get() { return s ? &v : nullptr; }
    void done() {
        if (s) wr32(s, v && iid ? wrap_by_iid(*iid, v) : 0);
    }
};

// Interface arrays: I* const* in (proxies -> host objects), I** out (wrapped back).
template <typename T>
struct ArrayArg;
template <typename I>
struct ArrayArg<I* const*> {
    static constexpr bool unsafe = false;
    uint32_t s, n;
    std::vector<I*> v;
    I* const* get() {
        if (!s) return nullptr;
        v.resize(n);
        for (uint32_t i = 0; i < n; ++i) v[i] = static_cast<I*>(to_host(rd32(s + 4 * i)));
        return v.data();
    }
    void done() {}
};
template <typename I>
struct ArrayArg<I**> {
    static constexpr bool unsafe = false;
    uint32_t s, n;
    std::vector<I*> v;
    I** get() {
        if (!s) return nullptr;
        v.assign(n, nullptr);
        return v.data();
    }
    void done() {
        if (!s) return;
        for (uint32_t i = 0; i < n; ++i) wr32(s + 4 * i, wrap(v[i]));
    }
};

// Generated ARRAY_PARAMS(I, M, {index, count_index}, ...).
struct ArraySpec {
    int index, count;
};
template <auto M>
struct ArrayParams {
    static constexpr std::array<ArraySpec, 0> v{};
};
#define ARRAY_PARAMS(I, M, ...)                               \
    template <>                                               \
    struct ArrayParams<&I::M> {                               \
        static constexpr ArraySpec list[] = {__VA_ARGS__};    \
        static constexpr std::array<ArraySpec, std::size(list)> v = std::to_array(list); \
    };
// Generated WIDE_PARAM(I, M): a 64-bit scalar takes two guest slots; the
// generic bridge assumes one, so such a method needs an OVERRIDE.
template <auto M>
struct WideParam : std::false_type {};
#define WIDE_PARAM(I, M) \
    template <>          \
    struct WideParam<&I::M> : std::true_type {};

template <auto M>
constexpr int array_count_index(int i) {
    for (const ArraySpec& a : ArrayParams<M>::v)
        if (a.index == i) return a.count;
    return -1;
}

// The converter for argument i of method M (type A, previous argument type P).
template <auto M, size_t I, typename A, typename P>
struct ArgFor {
    using type = std::conditional_t<
        (array_count_index<M>(int(I)) >= 0), ArrayArg<A>,
        std::conditional_t<std::is_same_v<A, void**> && std::is_same_v<P, const GUID&>, IidOutArg, Arg<A>>>;
};

template <typename... A>
struct TypeList {};
template <size_t I, typename... A>
using nth_t = std::tuple_element_t<I, std::tuple<A...>>;
template <size_t I, typename... A>
using prev_t = std::conditional_t<(I == 0), void, nth_t<(I == 0 ? 0 : I - 1), A...>>;

// ---------------------------------------------------------------------------
// Generic method bridge
// ---------------------------------------------------------------------------

template <typename R>
void set_result(X86* c, R r) {
    if constexpr (std::is_same_v<R, float>) fpush(c, double(r));
    else c->eax = uint32_t(r);
}

// A method taking a struct whose guest layout differs, with no OVERRIDE: log
// once and fail rather than pass a wrongly laid-out struct.
template <size_t NArgs>
void unsupported_call(X86* c) {
    static std::atomic<int> logged{0};
    if (logged++ < 10)
        rt::log("%s: unsupported method (layout-differing argument, needs an OVERRIDE), called from %08X", kBridge,
                rd32(c->esp));
    c->eax = uint32_t(kInvalidCall);
    c->esp += 4 * (2 + uint32_t(NArgs));
}

template <typename F, F M>
struct BridgeImpl;

template <typename C, typename R, typename... A, R (C::*M)(A...)>
struct BridgeImpl<R (C::*)(A...), M> {
    template <size_t I>
    using arg_t = typename ArgFor<M, I, nth_t<I, A...>, prev_t<I, A...>>::type;

    template <size_t I>
    static arg_t<I> make(X86* c) {
        const uint32_t s = rd32(c->esp + 8 + 4 * uint32_t(I));
        using T = arg_t<I>;
        if constexpr (array_count_index<M>(int(I)) >= 0) {
            return T{s, rd32(c->esp + 8 + 4 * uint32_t(array_count_index<M>(int(I))))};
        } else {
            return T{s};
        }
    }

    template <size_t... I>
    static void run(X86* c, std::index_sequence<I...>) {
        C* self = static_cast<C*>(to_host(rd32(c->esp + 4)));
        std::tuple<arg_t<I>...> args{make<I>(c)...};
        // IID outs read the IID argument just before them.
        (
            [&] {
                if constexpr (std::is_same_v<arg_t<I>, IidOutArg>)
                    std::get<I>(args).iid = &std::get<(I == 0 ? 0 : I - 1)>(args).get();
            }(),
            ...);
        if (!self) {
            c->eax = uint32_t(kInvalidCall);
        } else if constexpr (std::is_void_v<R>) {
            (self->*M)(std::get<I>(args).get()...);
        } else {
            set_result<R>(c, (self->*M)(std::get<I>(args).get()...));
        }
        (std::get<I>(args).done(), ...);
        c->esp += 4 * (2 + uint32_t(sizeof...(A)));  // return address, this, arguments
    }
    template <size_t... I>
    static constexpr bool any_unsafe(std::index_sequence<I...>) {
        return (arg_t<I>::unsafe || ...);
    }
    static void call(X86* c) { run(c, std::index_sequence_for<A...>{}); }
    static constexpr recomp_fn fn() {
        if constexpr (any_unsafe(std::index_sequence_for<A...>{}) || WideParam<M>::value)
            return &unsupported_call<sizeof...(A)>;
        else
            return &call;
    }
};

template <auto M>
struct Select {
    static constexpr recomp_fn fn = BridgeImpl<decltype(M), M>::fn();
};

// Overrides: Select<&I::Method> specialisations pointing at hand-written bridges.
#define OVERRIDE(I, M, F) \
    template <>           \
    struct Select<&I::M> { static constexpr recomp_fn fn = &F; };

// ---- Call census (DIRT2_COUNT_CALLS=1) ----
// Every generated slot counts its calls when enabled; every 10 s the totals so
// far are logged, most called first. What a host backend has to implement, and
// how hot each method is (the deko3d device, docs/SWITCH.md).

struct CallCounter {
    const char* name;
    std::atomic<uint64_t> n{0};
};

inline bool count_calls() {
    static const bool on = std::getenv("DIRT2_COUNT_CALLS") != nullptr;
    return on;
}

inline CallCounter* register_counter(const char* name) {
    static std::mutex mu;
    static std::vector<CallCounter*> all;
    std::lock_guard lk(mu);
    if (all.empty())
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                std::vector<std::pair<uint64_t, const char*>> v;
                {
                    std::lock_guard lk(mu);
                    for (CallCounter* k : all) v.emplace_back(k->n.load(std::memory_order_relaxed), k->name);
                }
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
                rt::log("%s calls: %zu methods", kBridge, v.size());
                for (size_t i = 0; i < v.size(); i += 12) {  // short lines: the log truncates at 2 KB
                    std::string line;
                    for (size_t j = i; j < v.size() && j < i + 12; ++j)
                        line += std::string(" ") + v[j].second + "=" + std::to_string(v[j].first);
                    rt::log("%s calls:%s", kBridge, line.c_str());
                }
            }
        }).detach();
    all.push_back(new CallCounter{name});
    return all.back();
}

template <size_t N>
struct SlotName {
    char s[N];
    constexpr SlotName(const char (&a)[N]) {
        for (size_t i = 0; i < N; ++i) s[i] = a[i];
    }
};

template <SlotName Name, recomp_fn F>
void counted(X86* c) {
    if (count_calls()) {
        static CallCounter* const k = register_counter(Name.s);
        k->n.fetch_add(1, std::memory_order_relaxed);
    }
    F(c);
}

#define SLOT(I, M) &counted<SlotName(#I "::" #M), Select<&I::M>::fn>


// ---- IUnknown ----

template <typename I>
void AddRefB(X86* c) {
    IUnknown* self = to_host(rd32(c->esp + 4));
    c->eax = self ? self->AddRef() : 0;
    c->esp += 8;
}
template <typename I>
void ReleaseB(X86* c) {
    const uint32_t g = rd32(c->esp + 4);
    IUnknown* self = to_host(g);
    const ULONG r = self ? self->Release() : 0;
    if (self && r == 0) {
        forget(g);
        on_release(g);
    }
    c->eax = r;
    c->esp += 8;
}
template <typename I>
void QI(X86* c) {
    IUnknown* self = to_host(rd32(c->esp + 4));
    const GUID& iid = *reinterpret_cast<const GUID*>(rt::host_ptr(rd32(c->esp + 8)));
    const uint32_t out = rd32(c->esp + 12);
    void* obj = nullptr;
    const HRESULT hr = self ? self->QueryInterface(iid, &obj) : E_NOINTERFACE;
    if (out) wr32(out, SUCCEEDED(hr) ? wrap_by_iid(iid, obj) : 0);
    c->eax = uint32_t(hr);
    c->esp += 16;
}

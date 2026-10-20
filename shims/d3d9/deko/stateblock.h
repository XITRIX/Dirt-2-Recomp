// D3D9 state blocks keep only the states selected by their type or recording.
// Each entry owns a value, a capture operation, and an ordinary device setter.
// Applying through setters also restores dirty tracking for the next GPU draw.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <type_traits>

namespace deko {
class Device;
class StateBlock final : public StubIDirect3DStateBlock9 {
public:
    explicit StateBlock(Device* device);
    bool capture_stream_offsets = true;
    STDMETHOD(GetDevice)(IDirect3DDevice9** out) override;
    STDMETHOD(Capture)() override;
    STDMETHOD(Apply)() override;
    enum Kind : uint32_t {
        Render, Sampler, TextureStage, Transform, Material, LightValue, LightEnabled,
        ClipPlane, Viewport, Scissor, Declaration, VertexProgram, PixelProgram,
        VertexFloat, VertexInt, VertexBool, PixelFloat, PixelInt, PixelBool,
        StreamSource, StreamFrequency, IndexSource, TextureBinding,
    };
    template<class T, class Get, class Set>
    HRESULT record(Kind kind, uint32_t index, T value, Get get, Set set) {
        auto saved = std::make_shared<T>(std::move(value));
        entries_[(uint64_t(kind) << 32) | index] = {
            [saved, get] { *saved = get(); },
            [saved, set] { set(*saved); }};
        return D3D_OK;
    }
private:
    Device* device_;
    struct Entry { std::function<void()> capture, apply; };
    std::map<uint64_t, Entry> entries_;
};

template<class T> std::shared_ptr<T> state_resource(T* object) {
    if (!object) return {};
    if constexpr (std::is_same_v<T, IDirect3DBaseTexture9>) texture_add_private(object);
    else object->T::AddRefPrivate();
    return std::shared_ptr<T>(object, [](T* p) {
        if constexpr (std::is_same_v<T, IDirect3DBaseTexture9>) texture_release_private(p);
        else p->T::ReleasePrivate();
    });
}
template<class T, size_t N> std::array<T, N> state_array(const T* data) {
    std::array<T,N> result;
    std::copy_n(data,N,result.data());
    return result;
}
} // namespace deko

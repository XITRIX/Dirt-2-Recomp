// GPU core of the D3D9-on-deko3d device (see gpu.h).
#include "gpu.h"

#include <switch.h>

#include <algorithm>
#include <cstring>

#include "recomp/host.h"

namespace deko {

namespace {

constexpr uint32_t kCmdMemSize = 8u << 20;        // per frame; more is added on demand
constexpr uint32_t kCmdExtraSize = 2u << 20;
constexpr uint32_t kTransientSize = 32u << 20;    // per frame
constexpr uint32_t kCodeSize = 8u << 20;          // shader code

uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

DkMemBlock make_block(uint32_t size, uint32_t flags) {
    DkMemBlockMaker m;
    dkMemBlockMakerDefaults(&m, gpu().device, align_up(size, DK_MEMBLOCK_ALIGNMENT));
    m.flags = flags;
    return dkMemBlockCreate(&m);
}

void debug_cb(void*, const char* context, DkResult result, const char* message) {
    rt::log("deko3d: %s: %s (result %d)", context, message ? message : "", int(result));
}

// Submits a command list and reports a queue in error (a GPU fault) once.
void submit(DkQueue q, DkCmdList list, const char* where) {
    dkQueueSubmitCommands(q, list);
    static bool reported = false;
    if (!reported && dkQueueIsInErrorState(q)) {
        reported = true;
        rt::log("deko3d: the queue is in an error state after %s (GPU fault)", where);
    }
}

// Waits for a fence; a wait over 2 s is logged (a GPU hang shows up in the log).
void wait_fence(DkFence* f, const char* what) {
    if (dkFenceWait(f, 2'000'000'000) == DkResult_Success) return;
    rt::log("deko3d: %s has not finished after 2 s: the GPU may be hung", what);
    dkFenceWait(f, -1);
    rt::log("deko3d: %s finished", what);
}

// The command buffer ran out of the frame's memory: give it another chunk.
void add_cmd_mem(void*, DkCmdBuf cmd, size_t min_size) {
    Gpu& g = gpu();
    const uint32_t size = align_up(std::max<uint32_t>(uint32_t(min_size), kCmdExtraSize), DK_MEMBLOCK_ALIGNMENT);
    DkMemBlock b = make_block(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    g.cur().cmd_extra.push_back(b);
    dkCmdBufAddMemory(cmd, b, 0, size);
}

}  // namespace

Gpu& gpu() {
    static Gpu g;
    return g;
}

// ---- Pool ----

Alloc Pool::alloc(uint32_t size, uint32_t align) {
    align = std::max<uint32_t>(align, 16);
    size = align_up(std::max<uint32_t>(size, 1), 16);
    if (size + align > block_size_) {  // dedicated block
        Block b{make_block(size, flags_), {}, true};
        if (!b.mem) return {};
        blocks_.push_back(std::move(b));
        return Alloc{blocks_.back().mem, 0, size};
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (Block& b : blocks_)
            for (auto it = b.free.begin(); it != b.free.end(); ++it) {
                const uint32_t start = align_up(it->first, align);
                const uint32_t end = it->first + it->second;
                if (start + size > end) continue;
                const uint32_t off = it->first, len = it->second;
                b.free.erase(it);
                if (start > off) b.free[off] = start - off;
                if (start + size < off + len) b.free[start + size] = off + len - start - size;
                return Alloc{b.mem, start, size};
            }
        Block b{make_block(block_size_, flags_), {}, false};
        if (!b.mem) return {};
        b.free[0] = block_size_;
        blocks_.push_back(std::move(b));
    }
    return {};
}

void Pool::free_now(const Alloc& a) {
    for (Block& b : blocks_) {
        if (b.mem != a.block) continue;
        if (b.dedicated) {
            dkMemBlockDestroy(b.mem);
            b = std::move(blocks_.back());
            blocks_.pop_back();
            return;
        }
        auto [it, ok] = b.free.emplace(a.offset, a.size);
        if (auto next = std::next(it); next != b.free.end() && it->first + it->second == next->first) {
            it->second += next->second;
            b.free.erase(next);
        }
        if (it != b.free.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                b.free.erase(it);
            }
        }
        return;
    }
}

// ---- Ring ----

void Ring::init(uint32_t flags, uint32_t size) {
    flags_ = flags;
    size_ = size;
    mem_ = make_block(size, flags);
}

Alloc Ring::alloc(uint32_t size, uint32_t align) {
    align = std::max<uint32_t>(align, 16);
    const uint32_t start = align_up(used_, align);
    if (start + size <= size_) {
        used_ = start + size;
        return Alloc{mem_, start, size};
    }
    DkMemBlock b = make_block(size, flags_);
    overflow_.push_back(b);
    return Alloc{b, 0, size};
}

void Ring::reset() {
    used_ = 0;
    for (DkMemBlock b : overflow_) dkMemBlockDestroy(b);
    overflow_.clear();
}

// ---- Ids ----

int Ids::take() {
    if (!free_.empty()) {
        const int id = free_.back();
        free_.pop_back();
        return id;
    }
    if (next_ >= count_) {
        rt::log("deko3d: out of descriptor slots (%u)", count_);
        return 0;
    }
    return int(next_++);
}

// ---- Gpu ----

bool Gpu::init() {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);  // depth 0..1, upper-left origin: D3D's conventions
    dm.cbDebug = debug_cb;
    device = dkDeviceCreate(&dm);
    if (!device) return false;

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, device);
    // No Zcull: its coarse per-tile depth went stale across the game's depth
    // target use and rejected whole 4x4 tiles of draws (the frontend's depth
    // MRT came out 72% empty, the focus pass then filled the screen with noise).
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_HighPrio | DkQueueFlags_DisableZcull;
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = 256u << 10;
    queue = dkQueueCreate(&qm);

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, device);
    cm.cbAddMem = add_cmd_mem;
    cmd = dkCmdBufCreate(&cm);

    code = make_block(kCodeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
    descriptors = make_block(kMaxImages * sizeof(DkImageDescriptor) + kMaxSamplers * sizeof(DkSamplerDescriptor),
                             DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    std::memset(dkMemBlockGetCpuAddr(descriptors), 0, dkMemBlockGetSize(descriptors));
    for (Frame& f : frames) {
        f.cmd_mem = make_block(kCmdMemSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
        f.transient.init(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, kTransientSize);
    }

    // Swap chain: 1920x1080 docked, 1280x720 handheld (fixed at start for now).
    const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
    swap_w = docked ? 1920 : 1280;
    swap_h = docked ? 1080 : 720;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, device);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = swap_w;
    lm.dimensions[1] = swap_h;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &lm);
    const uint32_t isize = align_up(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    swap_mem = make_block(isize * kSwapImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    DkImage const* ptrs[kSwapImages];
    for (int i = 0; i < kSwapImages; ++i) {
        dkImageInitialize(&swap_images[i], &layout, swap_mem, uint32_t(i) * isize);
        ptrs[i] = &swap_images[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, device, nwindowGetDefault(), ptrs, kSwapImages);
    swapchain = dkSwapchainCreate(&sm);

    dkCmdBufClear(cmd);
    dkCmdBufAddMemory(cmd, cur().cmd_mem, 0, kCmdMemSize);
    begin_list();
    rt::log("deko3d: device up, swap chain %ux%u (%s)", swap_w, swap_h, docked ? "docked" : "handheld");
    return swapchain != nullptr;
}

void Gpu::begin_list() {
    ++list_id;
    const DkGpuAddr base = dkMemBlockGetGpuAddr(descriptors);
    dkCmdBufBindImageDescriptorSet(cmd, base, kMaxImages);
    dkCmdBufBindSamplerDescriptorSet(cmd, base + kMaxImages * sizeof(DkImageDescriptor), kMaxSamplers);
    descriptors_dirty = true;
}

void Gpu::flush(bool wait, const char* what) {
    submit(queue, dkCmdBufFinishList(cmd), wait ? "a wait-for-idle submit" : "a flush");
    if (wait) {
        // dkQueueWaitIdle has no timeout: wait on a fence instead.
        // As deko3d's own waitIdle: signal, then flush (the signal's flush
        // argument only flushes caches; without dkQueueFlush nothing is submitted).
        DkFence f{};
        dkQueueSignalFence(queue, &f, true);
        dkQueueFlush(queue);
        wait_fence(&f, what);
    }
    else dkQueueFlush(queue);
    begin_list();
}

int Gpu::acquire_swap_image() { return dkQueueAcquireImage(queue, swapchain); }

void Gpu::end_frame(int present_slot) {
    submit(queue, dkCmdBufFinishList(cmd), "a frame");
    dkQueueSignalFence(queue, &cur().fence, true);
    cur().submitted = true;
    if (present_slot >= 0) dkQueuePresentImage(queue, swapchain, present_slot);
    ++frame_count;
    frame = (frame + 1) % kFrames;

    // Reuse the next frame's memory once its GPU work is done.
    Frame& f = cur();
    if (f.submitted) wait_fence(&f.fence, "the frame before last");
    f.submitted = false;
    for (auto& fn : f.on_done) fn();
    f.on_done.clear();
    f.transient.reset();
    for (DkMemBlock b : f.cmd_extra) dkMemBlockDestroy(b);
    f.cmd_extra.clear();
    dkCmdBufClear(cmd);
    dkCmdBufAddMemory(cmd, f.cmd_mem, 0, kCmdMemSize);
    begin_list();
}

// ---- GpuImage ----

bool GpuImage::create(DkImageType t, DkImageFormat f, uint32_t w, uint32_t h, uint32_t d, uint32_t lv, uint32_t ly,
                      uint32_t flags) {
    type = t;
    format = f;
    width = std::max<uint32_t>(w, 1);
    height = std::max<uint32_t>(h, 1);
    depth = std::max<uint32_t>(d, 1);
    levels = std::max<uint32_t>(lv, 1);
    layers = std::max<uint32_t>(ly, 1);
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, gpu().device);
    lm.type = t;
    lm.flags = flags;
    lm.format = f;
    lm.dimensions[0] = width;
    lm.dimensions[1] = height;
    lm.dimensions[2] = t == DkImageType_3D ? depth : (t == DkImageType_Cubemap ? 6 : layers);
    lm.mipLevels = levels;
    dkImageLayoutInitialize(&layout, &lm);
    mem = gpu().images.alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    if (!mem) return false;
    dkImageInitialize(&image, &layout, mem.block, mem.offset);
    return true;
}

void GpuImage::destroy() {
    if (!mem) return;
    const Alloc m = mem;
    const int id = desc;
    gpu().defer([m, id] {
        gpu().images.free_now(m);
        if (id >= 0) gpu().image_ids.give(id);
    });
    mem = {};
    desc = -1;
}

DkImageView GpuImage::view(uint32_t level, uint32_t layer, bool all, bool sampled) const {
    DkImageView v;
    dkImageViewDefaults(&v, &image);
    if (sampled)
        for (int i = 0; i < 4; ++i) v.swizzle[i] = swizzle[i];
    if (!all) {
        v.mipLevelOffset = uint8_t(level);
        v.mipLevelCount = 1;
        if (type == DkImageType_Cubemap) {
            v.type = DkImageType_2D;  // one face
            v.layerOffset = uint16_t(layer);
            v.layerCount = 1;
        }
    }
    return v;
}

int GpuImage::descriptor() {
    if (desc >= 0) return desc;
    Gpu& g = gpu();
    desc = g.image_ids.take();
    const DkImageView v = view(0, 0, true, true);
    dkImageDescriptorInitialize(&g.image_desc()[desc], &v, false, false);
    g.descriptors_dirty = true;
    return desc;
}

}  // namespace deko

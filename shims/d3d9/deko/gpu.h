// GPU core of the D3D9-on-deko3d device (Switch): the deko3d device and queue,
// memory pools, per-frame command and transient memory, the image and sampler
// descriptor tables, and the swap chain on the default NWindow.
//
// Frames: kFrames frames are in flight. Each has its own command memory and
// transient ring (staging for uploads, DISCARD buffer renames, UP draws); both
// are reused once the frame's fence has signalled, and resources freed during a
// frame are only returned to their pools then.
#pragma once

#include <deko3d.h>

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace deko {

constexpr int kFrames = 2;

// A suballocation of a memory block.
struct Alloc {
    DkMemBlock block = nullptr;
    uint32_t offset = 0, size = 0;
    explicit operator bool() const { return block != nullptr; }
    uint8_t* cpu() const { return static_cast<uint8_t*>(dkMemBlockGetCpuAddr(block)) + offset; }
    DkGpuAddr gpu() const { return dkMemBlockGetGpuAddr(block) + offset; }
};

// Memory of one kind (flags) in blocks of block_size, first-fit with coalescing.
class Pool {
public:
    Pool(uint32_t flags, uint32_t block_size) : flags_(flags), block_size_(block_size) {}
    Alloc alloc(uint32_t size, uint32_t align);
    void free_now(const Alloc& a);

private:
    struct Block {
        DkMemBlock mem;
        std::map<uint32_t, uint32_t> free;  // offset -> size
        bool dedicated = false;             // one allocation larger than block_size_
    };
    uint32_t flags_, block_size_;
    std::vector<Block> blocks_;
};

// Linear per-frame allocator; reset when the frame's fence has passed.
class Ring {
public:
    void init(uint32_t flags, uint32_t size);
    Alloc alloc(uint32_t size, uint32_t align);  // falls back to a dedicated block when full
    void reset();

private:
    DkMemBlock mem_ = nullptr;
    uint32_t size_ = 0, used_ = 0;
    std::vector<DkMemBlock> overflow_;
    uint32_t flags_ = 0;
};

// A slot table (image or sampler descriptors), ids freed after the GPU is done.
class Ids {
public:
    explicit Ids(uint32_t n) : next_(0), count_(n) {}
    int take();
    void give(int id) { free_.push_back(id); }

private:
    std::vector<int> free_;
    uint32_t next_, count_;
};

struct Gpu {
    DkDevice device = nullptr;
    DkQueue queue = nullptr;
    DkCmdBuf cmd = nullptr;
    DkMemBlock code = nullptr;  // shader code (the pack's DKSH code sections)
    uint32_t code_used = 0;

    Pool buffers{DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 16u << 20};
    Pool images{DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 32u << 20};
    Pool readback{DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached, 4u << 20};

    // Descriptors: the image table and the sampler table, bound for every list.
    static constexpr uint32_t kMaxImages = 8192, kMaxSamplers = 2048;
    DkMemBlock descriptors = nullptr;
    Ids image_ids{kMaxImages}, sampler_ids{kMaxSamplers};
    bool descriptors_dirty = true;
    DkImageDescriptor* image_desc() const { return static_cast<DkImageDescriptor*>(dkMemBlockGetCpuAddr(descriptors)); }
    DkSamplerDescriptor* sampler_desc() const {
        return reinterpret_cast<DkSamplerDescriptor*>(image_desc() + kMaxImages);
    }

    struct Frame {
        DkMemBlock cmd_mem = nullptr;
        std::vector<DkMemBlock> cmd_extra;
        Ring transient;
        DkFence fence{};
        bool submitted = false;
        std::vector<std::function<void()>> on_done;  // deferred frees
    };
    Frame frames[kFrames];
    int frame = 0;
    uint64_t frame_count = 0;
    uint64_t list_id = 0;  // bumped by begin_list: bound state must be re-recorded

    // Swap chain on nwindowGetDefault().
    static constexpr int kSwapImages = 3;
    DkImage swap_images[kSwapImages];
    DkMemBlock swap_mem = nullptr;
    DkSwapchain swapchain = nullptr;
    uint32_t swap_w = 0, swap_h = 0;

    bool init();
    Frame& cur() { return frames[frame]; }
    // Runs f once the current frame's GPU work is done (resource destruction).
    void defer(std::function<void()> f) { cur().on_done.push_back(std::move(f)); }
    // Records the per-list state (descriptor tables) at the start of a command list.
    void begin_list();
    // Submits what is recorded so far; with wait, blocks until the GPU is idle.
    void flush(bool wait);
    // Ends the frame: submit, signal its fence, present swap image `slot`, start the next frame.
    void end_frame(int present_slot);
    int acquire_swap_image();
};

Gpu& gpu();

// Image helpers: one layout + memory per image.
struct GpuImage {
    DkImage image{};
    DkImageLayout layout{};
    Alloc mem;
    int desc = -1;  // image descriptor id, -1 until first sampled
    DkImageFormat format = DkImageFormat_None;
    DkImageType type = DkImageType_2D;
    uint32_t width = 0, height = 0, depth = 1, levels = 1, layers = 1;
    DkImageSwizzle swizzle[4] = {DkImageSwizzle_Red, DkImageSwizzle_Green, DkImageSwizzle_Blue, DkImageSwizzle_Alpha};

    bool create(DkImageType t, DkImageFormat f, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t layers,
                uint32_t flags);
    void destroy();  // deferred: memory and descriptor are returned after the frame
    DkImageView view(uint32_t level = 0, uint32_t layer = 0, bool all = false) const;
    int descriptor();  // writes the image descriptor on first use
};

}  // namespace deko

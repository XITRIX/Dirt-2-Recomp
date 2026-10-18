// MetalFX spatial upscaling of the game's frame.
//
// DXVK calls our present hook (third_party/patches/dxvk/0002-present-hook)
// on its submission thread after the frame was submitted and just before
// vkQueuePresentKHR, with the D3D9 back buffer and the acquired swap chain
// image. When the back buffer is smaller than the screen (render scale below
// 1) and MetalFX is on, we take their Metal textures and the queue through
// VK_EXT_metal_objects and encode MTLFXSpatialScaler into the drawable on
// that queue, so it runs before MoltenVK's present. DXVK's own bilinear blit
// has already filled the drawable, so any skipped frame still shows correctly.
#include <TargetConditionals.h>
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#define VK_USE_PLATFORM_METAL_EXT 1
#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include "../platform.h"
#include "recomp/host.h"

extern "C" void dxvk_set_present_hook(void (*hook)(VkDevice, VkQueue, VkImage, VkExtent2D, VkImage, VkExtent2D));

namespace platform {

namespace {

std::atomic<bool> g_enabled{true};
bool g_supported = false;
std::string g_settings;  // <data>/metalfx.txt: "1" or "0"

struct Cache {
    id<MTLFXSpatialScaler> scaler = nil;
    id<MTLTexture> resolved = nil;  // single-sample copy of an MSAA back buffer
    id<MTLTexture> output = nil;    // scaler output, when the drawable cannot take it directly
    NSUInteger inW = 0, inH = 0, outW = 0, outH = 0;
    MTLPixelFormat inFormat = MTLPixelFormatInvalid, outFormat = MTLPixelFormatInvalid;
};
std::mutex g_mu;
Cache g_cache;

id<MTLTexture> export_texture(VkDevice dev, VkImage image) {
    VkExportMetalTextureInfoEXT tex = {VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT};
    tex.image = image;
    tex.plane = VK_IMAGE_ASPECT_PLANE_0_BIT;
    VkExportMetalObjectsInfoEXT info = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &tex};
    vkExportMetalObjectsEXT(dev, &info);
    return tex.mtlTexture;
}

id<MTLCommandQueue> export_queue(VkDevice dev, VkQueue queue) {
    VkExportMetalCommandQueueInfoEXT q = {VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT};
    q.queue = queue;
    VkExportMetalObjectsInfoEXT info = {VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &q};
    vkExportMetalObjectsEXT(dev, &info);
    return q.mtlCommandQueue;
}

void log_once(const char* what) {
    static std::atomic<int> n{0};
    if (n++ < 4) rt::log("metalfx: %s", what);
}

bool prepare(id<MTLDevice> dev, id<MTLTexture> src, id<MTLTexture> dst, NSUInteger inW, NSUInteger inH) {
    Cache& c = g_cache;
    const MTLPixelFormat inFormat = src.pixelFormat, outFormat = dst.pixelFormat;
    if (c.scaler && c.inW == inW && c.inH == inH && c.outW == dst.width && c.outH == dst.height &&
        c.inFormat == inFormat && c.outFormat == outFormat)
        return true;
    c = Cache{};
    MTLFXSpatialScalerDescriptor* d = [[MTLFXSpatialScalerDescriptor alloc] init];
    d.inputWidth = inW;
    d.inputHeight = inH;
    d.outputWidth = dst.width;
    d.outputHeight = dst.height;
    d.colorTextureFormat = inFormat;
    d.outputTextureFormat = outFormat;
    d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;  // gamma-encoded 8-bit frame
    c.scaler = [d newSpatialScalerWithDevice:dev];
    if (!c.scaler) {
        log_once("cannot create a spatial scaler for this frame format");
        return false;
    }
    if (src.sampleCount > 1) {
        MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:inFormat
                                                                                     width:inW
                                                                                    height:inH
                                                                                 mipmapped:NO];
        t.usage = MTLTextureUsageRenderTarget | c.scaler.colorTextureUsage;
        t.storageMode = MTLStorageModePrivate;
        c.resolved = [dev newTextureWithDescriptor:t];
    }
    if ((dst.usage & c.scaler.outputTextureUsage) != c.scaler.outputTextureUsage) {
        MTLTextureDescriptor* t = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:outFormat
                                                                                     width:dst.width
                                                                                    height:dst.height
                                                                                 mipmapped:NO];
        t.usage = c.scaler.outputTextureUsage;
        t.storageMode = MTLStorageModePrivate;
        c.output = [dev newTextureWithDescriptor:t];
    }
    c.inW = inW;
    c.inH = inH;
    c.outW = dst.width;
    c.outH = dst.height;
    c.inFormat = inFormat;
    c.outFormat = outFormat;
    rt::log("metalfx: upscaling %lux%lu -> %lux%lu%s%s", (unsigned long)inW, (unsigned long)inH,
            (unsigned long)dst.width, (unsigned long)dst.height, c.resolved ? " (MSAA resolved)" : "",
            c.output ? " (via intermediate)" : "");
    return true;
}

void present_hook(VkDevice device, VkQueue queue, VkImage srcImage, VkExtent2D srcExtent, VkImage dstImage,
                  VkExtent2D dstExtent) {
    if (!g_enabled.load(std::memory_order_relaxed)) return;
    if (srcExtent.width >= dstExtent.width && srcExtent.height >= dstExtent.height) return;  // not upscaling
    @autoreleasepool {
        id<MTLTexture> src = export_texture(device, srcImage), dst = export_texture(device, dstImage);
        id<MTLCommandQueue> mq = export_queue(device, queue);
        if (!src || !dst || !mq) {
            log_once("Metal objects not available");
            return;
        }
        std::lock_guard lk(g_mu);
        if (!prepare(mq.device, src, dst, srcExtent.width, srcExtent.height)) return;
        Cache& c = g_cache;
        id<MTLCommandBuffer> cb = [mq commandBuffer];
        cb.label = @"MetalFX upscale";
        id<MTLTexture> color = src;
        if (c.resolved) {
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = src;
            rp.colorAttachments[0].resolveTexture = c.resolved;
            rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
            rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            color = c.resolved;
        }
        c.scaler.colorTexture = color;
        c.scaler.inputContentWidth = srcExtent.width;
        c.scaler.inputContentHeight = srcExtent.height;
        c.scaler.outputTexture = c.output ? c.output : dst;
        [c.scaler encodeToCommandBuffer:cb];
        if (c.output) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:c.output toTexture:dst];
            [blit endEncoding];
        }
        [cb commit];
    }
}

}  // namespace

void metalfx_init(const std::string& data_dir) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (@available(macOS 13.0, iOS 16.0, *))
            g_supported = dev && [MTLFXSpatialScalerDescriptor supportsDevice:dev];
    }
    g_settings = data_dir + "/metalfx.txt";
    std::string v;
    if (std::ifstream in(g_settings); in >> v) g_enabled = v != "0";
    rt::log("metalfx: %s, %s", g_supported ? "supported" : "not supported", g_enabled ? "on" : "off");
    if (g_supported) dxvk_set_present_hook(present_hook);
}

bool metalfx_supported() { return g_supported; }
bool metalfx_enabled() { return g_supported && g_enabled; }

void set_metalfx_enabled(bool on) {
    g_enabled = on;
    std::ofstream(g_settings) << (on ? "1" : "0") << "\n";
    rt::log("metalfx: %s", on ? "on" : "off");
}

}  // namespace platform

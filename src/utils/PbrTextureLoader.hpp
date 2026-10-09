#pragma once

#include "renderer/VulkanContext.hpp"
#include "renderer/BindlessDescriptor.hpp"
#include "utils/EtexLoader.hpp"
#include <string>
#include <cstdint>
#include <functional>
#include <vector>

namespace eruption {

// Forward declaration for PbrTextureSlot::reset.
void cancelPendingPbrUploadsForImage(VulkanContext* ctx, VkImage image);

// One bindless texture slot plus the Vulkan resources that back it.
// Keeping the handles here lets renderers destroy the images when they shut
// down, avoiding VMA allocation leaks.
struct PbrTextureSlot {
    uint32_t index = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation alloc = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    bool valid() const { return index != 0 && image != VK_NULL_HANDLE && view != VK_NULL_HANDLE; }

    void reset(VulkanContext* ctx, BindlessDescriptor* bindless) {
        if (!ctx) return;
        if (bindless && index != 0) {
            bindless->freeSlot(index);
            index = 0;
        }
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(ctx->device(), view, nullptr);
            view = VK_NULL_HANDLE;
        }
        if (image != VK_NULL_HANDLE) {
            cancelPendingPbrUploadsForImage(ctx, image);
            vmaDestroyImage(ctx->allocator(), image, alloc);
            image = VK_NULL_HANDLE;
            alloc = VK_NULL_HANDLE;
        }
    }
};

struct PbrTextureSlots {
    PbrTextureSlot mrahw;
    PbrTextureSlot normal;

    bool valid() const { return mrahw.valid() && normal.valid(); }

    void reset(VulkanContext* ctx, BindlessDescriptor* bindless) {
        mrahw.reset(ctx, bindless);
        normal.reset(ctx, bindless);
    }
};

// CPU-side decoded PBR texture data. Used for background decoding so the main
// thread only has to perform the GPU upload.
struct PbrTextureData {
    std::vector<uint8_t> pixels;
    EtexData etex; // baked BC1/BC3 (preferred over pixels when valid)
    int width = 0;
    int height = 0;
    int channels = 0;
    bool valid() const { return (!pixels.empty() || etex.valid()) && width > 0 && height > 0; }
};

struct PbrTexturePathPair {
    std::string mrahw;
    std::string normal;
    bool valid = false;
};

// Resolve the cooked PBR map paths for an albedo path without uploading anything.
PbrTexturePathPair resolvePbrTexturePaths(const std::string& albedoPath);

bool loadIndustryStandardPbr(const std::string& albedoPath, PbrTextureData& outMrahw, PbrTextureData& outNormal);

// Load a PBR map from disk into CPU memory.
PbrTextureData loadPbrTextureData(const std::string& path);

// Decode the cooked _mrahw/_normal maps for these albedo texture names IN
// PARALLEL (JobSystem) into an internal cache. A following loadPbrTexturesForAlbedo()
// for the same name then skips the (expensive, serial) PNG decode and only does
// the GPU upload. Call once at the start of map loading; pair with
// clearPbrTextureDataPrewarm() when the map is torn down.
void prewarmPbrTextureData(const std::vector<std::string>& albedoNames);

// Par mrahw/normal JÁ PRONTO (sintetizado e comprimido em bloco pelo bake de
// texturas embutidas de GLB). É consultado EXATAMENTE onde a síntese entraria
// - depois do PBR cozido e do industry-standard - para não roubar a vez de um
// mapa autoral que exista em disco.
using BakedPbrProvider =
    std::function<bool(const std::string& albedoKey, PbrTextureData& outMrahw,
                       PbrTextureData& outNormal)>;
void setBakedPbrProvider(BakedPbrProvider provider);

// Derive mrahw+normal from an albedo texture name (searched on disk) when no
// cooked maps exist. Safe to call from worker threads; used by the background
// map loader so the render-thread fallback never has to synthesize mid-frame.
bool synthesizePbrForAlbedoKey(const std::string& albedoKey,
                               PbrTextureData& outMrahw, PbrTextureData& outNormal);

// Same synthesis, from pixels ALREADY IN MEMORY. Required for GLB-embedded
// textures: they have no file on disk, so both the cooked-map lookup and the
// disk-based synthesis fail and the material renders with flat fallback
// constants (this is why parana_field showed no PBR on ~90% of its
// materials). Safe to call from worker threads.
// organicHint: a imagem e' usada por material organico (grama, vegetacao,
// terra, neve) - a altura nao tenta achar rede de rejunte nela.
bool synthesizePbrFromPixels(const std::vector<uint8_t>& pixels, int width, int height,
                             int channels,
                             PbrTextureData& outMrahw, PbrTextureData& outNormal,
                             bool organicHint = false);

// Last-resort source of albedo pixels for names that exist only inside a GLB
// (no file on disk). The Engine registers one that searches the current map's
// embedded textures; without it, embedded-texture materials get no PBR at all.
using EmbeddedAlbedoProvider =
    std::function<bool(const std::string& name, std::vector<uint8_t>& outPixels,
                       int& outW, int& outH, int& outChannels)>;
void setEmbeddedAlbedoProvider(EmbeddedAlbedoProvider provider);

// Liga/desliga a SÍNTESE de PBR (derivada do albedo). Cobrir todos os
// materiais custa ~1,9 ms de GPU nesta cena (cada material passa a amostrar
// mrahw + normal); em hardware fraco o preset pode abrir mão do relevo nos
// materiais sem PBR autoral. Não afeta PBR cozido, que é sempre usado.
void setPbrSynthesisEnabled(bool enabled);
bool pbrSynthesisEnabled();
void clearPbrTextureDataPrewarm();

// Upload already-decoded PBR maps to the GPU bindless descriptor.
PbrTextureSlots uploadPbrTexturesFromData(VulkanContext* ctx,
                                          BindlessDescriptor* bindless,
                                          VkSampler sampler,
                                          const PbrTextureData& mrahw,
                                          const PbrTextureData& normal);

// Try to load the pre-cooked PBR maps for a given albedo texture path.
// Maps are stored next to the original texture layout because they are shared
// across maps like a global palette:
//   assets/data/texture/<relative-dir>/<stem>_mrahw.png
//   assets/data/texture/<relative-dir>/<stem>_normal.png
// Returns zeroed slots when the cache does not exist or fails to load.
PbrTextureSlots loadPbrTexturesForAlbedo(VulkanContext* ctx,
                                         BindlessDescriptor* bindless,
                                         VkSampler sampler,
                                         const std::string& albedoPath);

// Flush any pending PBR GPU uploads in a single command-buffer submit.
// Call this after a batch of loadPbrTexturesForAlbedo() calls (e.g. at the end
// of map loading) to avoid one synchronous submit per texture.
// NOTE: blocks the calling thread on a fence until the GPU drains everything
// (fine behind the synchronous-boot loading screen, NOT during a live warp -
// warps drain the queue incrementally via recordPendingPbrUploads instead).
void flushPbrTextureUploads(VulkanContext* ctx);

// Number of PBR uploads still queued (a warp's map swap must wait for zero).
size_t pendingPbrUploadCount();

// Record up to maxItems queued PBR uploads into an already-recording frame
// command buffer - no submit, no fence, so the render thread never stalls.
// The staging buffers are appended to stagingOut, whose owner must keep them
// alive until the GPU has executed the frame (MapContext::stagingBuffers is
// freed via the deferred-delete path, which satisfies that).
void recordPendingPbrUploads(VulkanContext* ctx, VkCommandBuffer cmd, size_t maxItems,
                             std::vector<std::pair<VkBuffer, VmaAllocation>>& stagingOut);

// Cancel any queued GPU upload for a given image and free its staging buffer.
// Called by PbrTextureSlot::reset before destroying the image so we do not
// submit a copy into a destroyed handle.
void cancelPendingPbrUploadsForImage(VulkanContext* ctx, VkImage image);

} // namespace eruption

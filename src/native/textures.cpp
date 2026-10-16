#include "textures.h"

#include <atomic>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

namespace bardown::native {

namespace {

namespace xenos = rex::graphics::xenos;
using rex::graphics::FormatInfo;
using rex::graphics::TextureInfo;
using xenos::TextureFormat;

// a guest format as Vulkan holds it. `channel` is where each guest component (x y z w) ends up in the Vulkan
// texel, for the packed 16-bit formats whose bit order Vulkan names the other way round
struct Format {
  VkFormat vk;
  VkComponentSwizzle channel[4];
};
constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B,
                             A = VK_COMPONENT_SWIZZLE_A;

bool HostFormat(TextureFormat format, Format& out) {
  switch (format) {
    case TextureFormat::k_8:
    case TextureFormat::k_8_A:
    case TextureFormat::k_8_B:
      out = {VK_FORMAT_R8_UNORM, {R, R, R, R}};
      return true;
    case TextureFormat::k_8_8:
      out = {VK_FORMAT_R8G8_UNORM, {R, G, G, G}};
      return true;
    case TextureFormat::k_8_8_8_8:
    case TextureFormat::k_8_8_8_8_A:
      out = {VK_FORMAT_R8G8B8A8_UNORM, {R, G, B, A}};
      return true;
    case TextureFormat::k_2_10_10_10:
      out = {VK_FORMAT_A2B10G10R10_UNORM_PACK32, {R, G, B, A}};
      return true;
    case TextureFormat::k_5_6_5:
      out = {VK_FORMAT_R5G6B5_UNORM_PACK16, {B, G, R, R}};
      return true;
    case TextureFormat::k_1_5_5_5:
      out = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, {B, G, R, A}};
      return true;
    case TextureFormat::k_4_4_4_4:
      out = {VK_FORMAT_R4G4B4A4_UNORM_PACK16, {A, B, G, R}};
      return true;
    case TextureFormat::k_DXT1:
      out = {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, {R, G, B, A}};
      return true;
    case TextureFormat::k_DXT2_3:
      out = {VK_FORMAT_BC2_UNORM_BLOCK, {R, G, B, A}};
      return true;
    case TextureFormat::k_DXT4_5:
      out = {VK_FORMAT_BC3_UNORM_BLOCK, {R, G, B, A}};
      return true;
    case TextureFormat::k_DXN:
      out = {VK_FORMAT_BC5_UNORM_BLOCK, {R, G, G, G}};
      return true;
    case TextureFormat::k_DXT5A:
      out = {VK_FORMAT_BC4_UNORM_BLOCK, {R, R, R, R}};
      return true;
    case TextureFormat::k_16:
      out = {VK_FORMAT_R16_UNORM, {R, R, R, R}};
      return true;
    case TextureFormat::k_16_16:
      out = {VK_FORMAT_R16G16_UNORM, {R, G, G, G}};
      return true;
    case TextureFormat::k_16_16_16_16:
      out = {VK_FORMAT_R16G16B16A16_UNORM, {R, G, B, A}};
      return true;
    case TextureFormat::k_16_FLOAT:
      out = {VK_FORMAT_R16_SFLOAT, {R, R, R, R}};
      return true;
    case TextureFormat::k_16_16_FLOAT:
      out = {VK_FORMAT_R16G16_SFLOAT, {R, G, G, G}};
      return true;
    case TextureFormat::k_16_16_16_16_FLOAT:
      out = {VK_FORMAT_R16G16B16A16_SFLOAT, {R, G, B, A}};
      return true;
    case TextureFormat::k_32_FLOAT:
      out = {VK_FORMAT_R32_SFLOAT, {R, R, R, R}};
      return true;
    case TextureFormat::k_32_32_FLOAT:
      out = {VK_FORMAT_R32G32_SFLOAT, {R, G, G, G}};
      return true;
    case TextureFormat::k_32_32_32_32_FLOAT:
      out = {VK_FORMAT_R32G32B32A32_SFLOAT, {R, G, B, A}};
      return true;
    default:
      return false;
  }
}

// the fetch constant's swizzle (3 bits per output: 0-3 a component, 4 zero, 5 one) on top of where the
// format put each component
VkComponentMapping ViewSwizzle(uint32_t swizzle, const Format& format) {
  VkComponentSwizzle out[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t s = (swizzle >> (3 * i)) & 7;
    out[i] = s < 4 ? format.channel[s] : s == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
  }
  return {out[0], out[1], out[2], out[3]};
}

struct Texture {
  Image image;
  Heap heap = Heap::k2D;
  uint32_t index = 0;
  uint32_t base = 0, size = 0;  // the guest data it came from
  uint32_t mip_base = 0, mip_size = 0;
  // one the game rewrites every frame: read again at the swap rather than when the draw was issued
  bool late = false;
  // a resolved image: the swizzle it was written in, and views of it through others
  uint32_t dest_swizzle = 0;
  std::vector<std::pair<uint32_t, std::pair<VkImageView, uint32_t>>> views;
  uint64_t hash = 0;
  uint64_t refreshed = UINT64_MAX;  // the frame it was last read again for
  // a resolved image: the texture the game declared for it (guest size and format), to tell when a later bind at
  // the same address is something else the game has since put there
  uint32_t guest_w = 0, guest_h = 0, guest_format = 0;
  xenos::xe_gpu_texture_fetch_t fetch = {};
  Format format = {};
};

struct Cache {
  std::mutex mutex;  // binds come from the game's worker threads too
  std::unordered_map<uint64_t, std::unique_ptr<Texture>> textures;  // by what the fetch constant says
  std::unordered_map<uint32_t, std::vector<uint64_t>> by_base;       // to drop them when the data changes
  std::unordered_map<uint32_t, std::unique_ptr<Texture>> resolved;   // render targets copied out, by address
  std::unordered_map<uint64_t, uint32_t> samplers;

  // pages the CPU wrote since, reported from whichever guest thread wrote them
  rex::memory::Memory* memory = nullptr;
  std::mutex written_mutex;
  std::vector<std::pair<uint32_t, uint32_t>> written;
  std::atomic<bool> any_written{false};
  bool trace = false;
  uint64_t loads = 0;
  std::chrono::steady_clock::duration load_time{};
  std::unordered_map<uint32_t, uint32_t> reloads;  // by base
  uint64_t late_changed = 0;
  uint64_t resolved_mismatch = 0;  // binds that found a resolved image declared as another texture
  uint64_t frame = 0;                // swaps assembled
  std::vector<std::pair<uint64_t, std::unique_ptr<Texture>>> resolved_dropped;  // by the frame they were dropped in
};
Cache c;

std::pair<uint32_t, uint32_t> OnWritten(void*, uint32_t start, uint32_t length, bool) {
  {
    std::lock_guard lock(c.written_mutex);
    c.written.emplace_back(start, length);
  }
  c.any_written.store(true, std::memory_order_release);
  // only the pages written stop being watched. "anything" let the runtime unwatch up to 4 MB around them while just
  // the textures on those pages were dropped, so the rest kept a stale copy and lost their watch: later loads into
  // that memory (portraits, faces) went unseen
  return {start, length};
}

void Retire(std::unordered_map<uint64_t, std::unique_ptr<Texture>>::iterator it) {
  RetireImage(it->second->image);
  HeapFree(it->second->heap, it->second->index);
  c.textures.erase(it);
}

// a resolved image dropped mid-frame can still be the destination of a resolve recorded earlier in this frame (or
// by a worker a frame ahead), replayed at the swap: it's kept whole until those frames are assembled
void RetireResolved(std::unordered_map<uint32_t, std::unique_ptr<Texture>>::iterator it) {
  c.resolved_dropped.emplace_back(c.frame, std::move(it->second));
  c.resolved.erase(it);
}

void FreeResolved(std::unique_ptr<Texture>& texture) {
  Texture& t = *texture;
  RetireImage(t.image);
  HeapFree(Heap::k2D, t.index);
  for (auto& [s, v] : t.views) {
    Image view_only;
    view_only.view = v.first;
    RetireImage(view_only);
    HeapFree(Heap::k2D, v.second);
  }
  texture.reset();
}

// drops every texture whose data the CPU wrote over
void DropWritten() {
  if (!c.any_written.exchange(false, std::memory_order_acquire)) return;
  std::vector<std::pair<uint32_t, uint32_t>> written;
  {
    std::lock_guard lock(c.written_mutex);
    written.swap(c.written);
  }
  for (auto it = c.textures.begin(); it != c.textures.end();) {
    const Texture& t = *it->second;
    const bool hit = !t.late && std::any_of(written.begin(), written.end(), [&](const auto& w) {
      return (t.base < w.first + w.second && w.first < t.base + t.size) ||
             (t.mip_size && t.mip_base < w.first + w.second && w.first < t.mip_base + t.mip_size);
    });
    if (hit) {
      auto next = std::next(it);
      Retire(it);
      it = next;
    } else {
      ++it;
    }
  }
  // a resolved image is what the GPU left there; once the CPU writes over it, memory holds something else
  for (auto it = c.resolved.begin(); it != c.resolved.end();) {
    const Texture& t = *it->second;
    const bool hit = t.size && std::any_of(written.begin(), written.end(), [&](const auto& w) {
      return t.base < w.first + w.second && w.first < t.base + t.size;
    });
    auto next = std::next(it);
    if (hit) RetireResolved(it);
    it = next;
  }
}

// the parts of the fetch constant that decide the texture's data and look (not how it's sampled)
uint64_t TextureKey(const uint32_t fetch[6]) {
  const uint32_t words[5] = {fetch[0] & 0xFFC00003u, fetch[1], fetch[2], fetch[3] & 0x7FFEu, fetch[5]};
  return XXH3_64bits(words, sizeof(words)) ^ (uint64_t(fetch[4] & 0x3FCu) << 52);
}

VkSamplerAddressMode Address(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case xenos::ClampMode::kMirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case xenos::ClampMode::kClampToBorder:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

uint32_t SamplerFor(const xenos::xe_gpu_texture_fetch_t& f) {
  // the 360 can't filter 32-bit float textures and point-samples them whatever the fetch constant says; the
  // crowd's bone palettes depend on it
  const bool unfilterable = f.format == TextureFormat::k_32_FLOAT || f.format == TextureFormat::k_32_32_FLOAT ||
                            f.format == TextureFormat::k_32_32_32_32_FLOAT;
  const uint64_t key = uint64_t(f.dword_0 >> 10 & 0x1FF) | uint64_t(f.dword_3 >> 19 & 0x3FF) << 9 |
                       uint64_t(f.dword_5 & 3) << 19 | uint64_t(unfilterable) << 21;
  if (auto it = c.samplers.find(key); it != c.samplers.end()) return it->second;
  VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = f.mag_filter == xenos::TextureFilter::kPoint || unfilterable ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  info.minFilter = f.min_filter == xenos::TextureFilter::kPoint || unfilterable ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  info.mipmapMode = f.mip_filter == xenos::TextureFilter::kLinear && !unfilterable ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  info.addressModeU = Address(f.clamp_x);
  info.addressModeV = Address(f.clamp_y);
  info.addressModeW = Address(f.clamp_z);
  info.maxLod = f.mip_filter == xenos::TextureFilter::kBaseMap ? 0.0f : VK_LOD_CLAMP_NONE;
  info.borderColor = f.border_color == xenos::BorderColor::k_ABGR_White ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                     : f.border_color == xenos::BorderColor::k_ABGR_Black
                         ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  if (!unfilterable && f.aniso_filter != xenos::AnisoFilter::kDisabled &&
      f.aniso_filter != xenos::AnisoFilter::kUseFetchConst) {
    info.anisotropyEnable = VK_TRUE;
    info.maxAnisotropy = float(1u << (uint32_t(f.aniso_filter) - 1));
  }
  VkSampler sampler = VK_NULL_HANDLE;
  if (!Check(Api().vkCreateSampler(Api().device, &info, nullptr, &sampler), "sampler")) return 0;
  const uint32_t index = HeapAdd(Heap::kSampler, VK_NULL_HANDLE, sampler);
  c.samplers.emplace(key, index);
  return index;
}

// BARDOWN_NATIVE_VERIFY: textures held from guest memory that changed there without a write notice, i.e. drawn
// stale. checked from the swap every 600 frames; the hash is taken at load only when this is on
const bool verify = std::getenv("BARDOWN_NATIVE_VERIFY") != nullptr;

uint64_t GuestHash(const Texture& t) {
  uint64_t hash = XXH3_64bits(Physical(t.base), t.size);
  if (t.mip_size) hash ^= XXH3_64bits(Physical(t.mip_base), t.mip_size) * 0x9E3779B97F4A7C15ull;
  return hash;
}

// copies a texture out of guest memory, every mip level and, for a cube, every face, untiled and in host byte
// order, and has it uploaded: into a new image, or over the one a late texture already has
// the guest memory a texture's base level occupies, worked out the way Fill does
bool GuestRange(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t& base, uint32_t& size) {
  TextureInfo info;
  if (!TextureInfo::Prepare(fetch, &info)) return false;
  const bool cube = info.dimension == xenos::DataDimension::kCube;
  namespace tu = rex::graphics::texture_util;
  uint32_t base_page, mip_page, min_level, max_level;
  tu::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page, &mip_page, &min_level,
                                       &max_level);
  if (!base_page) return false;
  const auto layout = tu::GetGuestTextureLayout(info.dimension, fetch.pitch, info.width + 1, info.height + 1,
                                                cube ? 6 : 1, info.is_tiled, info.format, fetch.packed_mips, true, 0);
  base = base_page << 12;
  size = layout.base.level_data_extent_bytes;
  return size != 0;
}

bool Fill(Texture& t, bool create) {
  const xenos::xe_gpu_texture_fetch_t& fetch = t.fetch;
  const Format& format = t.format;
  TextureInfo info;
  if (!TextureInfo::Prepare(fetch, &info)) return false;
  const bool cube = info.dimension == xenos::DataDimension::kCube;
  if (!cube && (info.dimension != xenos::DataDimension::k2DOrStacked || info.is_stacked)) return false;
  const FormatInfo* fi = info.format_info();
  const uint32_t width = info.width + 1, height = info.height + 1, faces = cube ? 6 : 1;
  // which levels exist and where, the way the emulator's loader works them out (the older TextureInfo mip
  // addressing puts small and non-square mip tails in the wrong place)
  namespace tu = rex::graphics::texture_util;
  uint32_t base_page, mip_page, min_level, max_level;
  tu::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page, &mip_page, &min_level,
                                       &max_level);
  if (!base_page) return false;  // only mips: not something this game was seen doing
  const uint32_t levels = std::min(max_level + 1, 13u);
  const auto layout = tu::GetGuestTextureLayout(info.dimension, fetch.pitch, width, height, faces, info.is_tiled,
                                                info.format, fetch.packed_mips, true, levels - 1);
  const uint32_t block_bytes = fi->bytes_per_block();
  const auto blocks = [&](uint32_t texels, uint32_t block) { return (texels + block - 1) / block; };

  size_t bytes = 0;
  for (uint32_t level = 0; level < levels; ++level)
    bytes += ((size_t(blocks(std::max(1u, width >> level), fi->block_width)) *
                   blocks(std::max(1u, height >> level), fi->block_height) * block_bytes +
               15) & ~size_t(15)) *
             faces;

  auto frame = FrameLock();  // the staging space and the upload commands have to land in the same frame
  Upload staging = Allocate(bytes, 16);
  std::vector<VkBufferImageCopy> copies;
  size_t at = 0;
  const auto endian = info.endianness;
  for (uint32_t level = 0; level < levels; ++level) {
    if (level && !mip_page) break;
    const uint32_t stored = std::min(level, layout.packed_level);  // levels in the packed tail share one
    const auto& level_layout = level ? layout.mips[stored] : layout.base;
    const uint32_t address = level ? (mip_page << 12) + layout.mip_offsets_bytes[stored] : base_page << 12;
    uint32_t offset_x = 0, offset_y = 0;  // in blocks, inside the packed tail
    if (level >= layout.packed_level) {
      uint32_t offset_z;
      tu::GetPackedMipOffset(width, height, 1, info.format, level, offset_x, offset_y, offset_z);
    }
    const uint32_t lw = std::max(1u, width >> level), lh = std::max(1u, height >> level);
    const uint32_t bx = blocks(lw, fi->block_width), by = blocks(lh, fi->block_height);
    const uint32_t pitch_blocks = level_layout.row_pitch_bytes / block_bytes;
    const uint32_t face_stride = level_layout.array_slice_stride_bytes;
    for (uint32_t face = 0; face < faces; ++face) {
      const uint8_t* src = Physical(address + face * face_stride);
      uint8_t* dst = staging.host + at;
      if (info.is_tiled) {
        rex::graphics::texture_conversion::UntileInfo untile = {};
        untile.offset_x = offset_x;
        untile.offset_y = offset_y;
        untile.width = bx;
        untile.height = by;
        untile.input_pitch = pitch_blocks;
        untile.output_pitch = bx;
        untile.input_format_info = fi;
        untile.output_format_info = fi;
        untile.copy_callback = [endian](void* out, const void* in, size_t n) {
          rex::graphics::texture_conversion::CopySwapBlock(endian, out, in, n);
        };
        rex::graphics::texture_conversion::Untile(dst, src, &untile);
      } else {
        const uint32_t pitch = level_layout.row_pitch_bytes;
        for (uint32_t y = 0; y < by; ++y)
          rex::graphics::texture_conversion::CopySwapBlock(endian, dst + size_t(y) * bx * block_bytes,
                                                           src + size_t(offset_y + y) * pitch + offset_x * block_bytes,
                                                           size_t(bx) * block_bytes);
      }
      VkBufferImageCopy copy = {};
      copy.bufferOffset = staging.offset + at;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
      copy.imageExtent = {lw, lh, 1};
      copies.push_back(copy);
      at += (size_t(bx) * by * block_bytes + 15) & ~size_t(15);
    }
  }
  if (copies.empty()) return false;
  const uint32_t loaded_levels = copies.back().imageSubresource.mipLevel + 1;

  if (create) {
    if (!MakeImage(t.image, width, height, format.vk, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   loaded_levels, ViewSwizzle(fetch.swizzle, format), cube ? ImageKind::kCube : ImageKind::k2D))
      return false;
    t.heap = cube ? Heap::kCube : Heap::k2D;
    t.index = HeapAdd(t.heap, t.image.view);
    t.base = base_page << 12;
    t.size = layout.base.level_data_extent_bytes;
    if (mip_page && levels > 1) {
      t.mip_base = mip_page << 12;
      t.mip_size = layout.mips_total_extent_bytes;
    }
  }
  VkCommandBuffer cb = UploadCommands();
  Transition(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  Api().vkCmdCopyBufferToImage(cb, staging.buffer, t.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(copies.size()), copies.data());
  Transition(cb, t.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return true;
}

std::unique_ptr<Texture> Load(const xenos::xe_gpu_texture_fetch_t& fetch, const Format& format, bool late) {
  auto t = std::make_unique<Texture>();
  t->fetch = fetch;
  t->format = format;
  if (!Fill(*t, true)) return nullptr;
  // small linear textures are data the game rewrites in place (the crowd's animation palettes), and a write the
  // watch misses would leave a stale copy for good: those are checked against memory every frame they're used
  t->late = late || (!fetch.tiled && t->size + t->mip_size <= 256 * 1024);
  if (t->late || verify) t->hash = GuestHash(*t);
  if (!t->late && c.memory) {
    // tell us when the CPU writes it again
    c.memory->EnablePhysicalMemoryAccessCallbacks(t->base, t->size, true, false);
    if (t->mip_size) c.memory->EnablePhysicalMemoryAccessCallbacks(t->mip_base, t->mip_size, true, false);
  }
  return t;
}

}  // namespace

void TextureTrace(bool on) { c.trace = on; }

void TexturesFrameDone() {
  std::lock_guard lock(c.mutex);
  ++c.frame;
  auto& dropped = c.resolved_dropped;
  auto keep = std::partition(dropped.begin(), dropped.end(), [](const auto& d) { return d.first + 2 >= c.frame; });
  for (auto it = keep; it != dropped.end(); ++it) FreeResolved(it->second);
  dropped.erase(keep, dropped.end());
}

uint32_t PointSampler() {
  std::lock_guard lock(c.mutex);
  static uint32_t index = [] {
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = VK_FILTER_NEAREST;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = info.addressModeV = info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (!Check(Api().vkCreateSampler(Api().device, &info, nullptr, &sampler), "sampler")) return 0u;
    return HeapAdd(Heap::kSampler, VK_NULL_HANDLE, sampler);
  }();
  return index;
}

uint64_t TextureLoads() {
  std::lock_guard lock(c.mutex);
  return c.loads;
}

double TakeTextureLoadTime() {
  std::lock_guard lock(c.mutex);
  const double ms = std::chrono::duration<double, std::milli>(c.load_time).count();
  c.load_time = {};
  return ms;
}

void TexturesInit(rex::memory::Memory* memory) {
  c.memory = memory;
  memory->RegisterPhysicalMemoryInvalidationCallback(OnWritten, nullptr);
}

TextureBinding BindTexture(const uint32_t words[6], bool vertex) {
  std::lock_guard lock(c.mutex);
  DropWritten();
  xenos::xe_gpu_texture_fetch_t fetch;
  std::memcpy(&fetch, words, sizeof(fetch));
  TextureBinding out;
  if (fetch.type != xenos::FetchConstantType::kTexture) return out;
  out.sampler = SamplerFor(fetch);

  const uint32_t base = fetch.base_address << 12;
  if (c.trace)
    REXLOG_INFO("[trace] bind {:08X} fmt {} {}x{} resolved {}", base, uint32_t(fetch.format), fetch.size_2d.width + 1,
                fetch.size_2d.height + 1, c.resolved.count(base));
  if (auto it = c.resolved.find(base); it != c.resolved.end()) {
    {
      const Texture& r = *it->second;
      const uint32_t w = fetch.size_2d.width + 1, h = fetch.size_2d.height + 1, f = uint32_t(fetch.format);
      if (r.guest_w && (w != r.guest_w || h != r.guest_h || f != r.guest_format)) {
        ++c.resolved_mismatch;
        static int logged = 0;
        if (logged < 20 && ++logged)
          REXLOG_INFO("[native] bind at resolved {:08X}: asks {}x{} format {}, resolved as {}x{} format {}; "
                      "reading memory instead", base, w, h, f, r.guest_w, r.guest_h, r.guest_format);
        RetireResolved(it);
      }
    }
  }
  if (auto it = c.resolved.find(base); it != c.resolved.end()) {
    // the 360 wrote the target in the destination header's channel order and this fetch reads that memory
    // through its own swizzle; the image holds the target as it was, so read through the difference. the
    // same swizzle (the usual case) is the image as it is
    Texture& t = *it->second;
    const uint32_t swizzle = fetch.swizzle;
    if (swizzle == t.dest_swizzle || Aspect(t.image.format) != VK_IMAGE_ASPECT_COLOR_BIT) {
      out.index = t.index;
      return out;
    }
    for (const auto& [s, v] : t.views)
      if (s == swizzle) {
        out.index = v.second;
        return out;
      }
    constexpr VkComponentSwizzle kChannel[4] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkComponentSwizzle map[4];
    for (int i = 0; i < 4; ++i) {
      const uint32_t f = swizzle >> (3 * i) & 7;
      map[i] = f == 5 || f == 7 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
      for (int j = 0; f < 4 && j < 4; ++j)
        if ((t.dest_swizzle >> (3 * j) & 7) == f) map[i] = kChannel[j];
    }
    const VkImageView view = MakeView(t.image, {map[0], map[1], map[2], map[3]});
    const uint32_t index = view ? HeapAdd(Heap::k2D, view) : t.index;
    if (view) t.views.push_back({swizzle, {view, index}});
    out.index = index;
    return out;
  }
  const uint64_t key = TextureKey(words);
  if (auto it = c.textures.find(key); it != c.textures.end()) {
    out.heap = it->second->heap;
    out.index = it->second->index;
    if (Texture& t = *it->second; vertex && !t.late) {
      t.late = true;
      t.hash = GuestHash(t);
    }
    if (it->second->late) out.late = key;
    return out;
  }
  Format format;
  if (!HostFormat(fetch.format, format)) {
    static uint64_t warned = 0;
    if (!(warned & (1ull << uint32_t(fetch.format))))
      REXLOG_WARN("[native] texture format {} isn't handled yet", uint32_t(fetch.format));
    warned |= 1ull << uint32_t(fetch.format);
    return out;
  }
  // written over again and again: the game fills it for the frame it's drawing, maybe after issuing the draw
  const bool late = vertex || c.reloads[base] >= 3;
  const auto start = std::chrono::steady_clock::now();
  auto t = Load(fetch, format, late);
  c.load_time += std::chrono::steady_clock::now() - start;
  if (!t) return out;
  ++c.loads;
  ++c.reloads[base];
  if (t->late) out.late = key;
  out.heap = t->heap;
  out.index = t->index;
  c.by_base[t->base].push_back(key);
  c.textures.emplace(key, std::move(t));
  return out;
}

void RefreshLate(uint64_t key) {
  std::lock_guard lock(c.mutex);
  auto it = c.textures.find(key);
  if (it == c.textures.end()) return;
  Texture& t = *it->second;
  const uint64_t frame = FrameNumber();
  if (t.refreshed == frame) return;
  t.refreshed = frame;
  const uint64_t hash = GuestHash(t);
  if (hash == t.hash) return;
  t.hash = hash;
  ++c.late_changed;
  Fill(t, false);
}

uint64_t CheckStale() {
  if (!verify) return 0;
  std::lock_guard lock(c.mutex);
  DropWritten();
  uint64_t stale = 0;
  for (auto& [key, texture] : c.textures) {
    Texture& t = *texture;
    if (t.late || !t.hash || GuestHash(t) == t.hash) continue;
    if (++stale <= 8)
      REXLOG_INFO("[native] stale texture {:08X} ({} bytes, mips {:08X} {} bytes) format {} {}x{}", t.base, t.size,
                  t.mip_base, t.mip_size, uint32_t(t.fetch.format), t.fetch.size_2d.width + 1,
                  t.fetch.size_2d.height + 1);
    t.hash = GuestHash(t);  // counted once
  }
  return stale;
}

uint64_t TakeLateChanges() {
  std::lock_guard lock(c.mutex);
  const uint64_t changed = c.late_changed;
  c.late_changed = 0;
  return changed;
}

void TextureWritten(uint32_t physical_address) {
  std::lock_guard lock(c.mutex);
  if (auto r = c.resolved.find(physical_address & 0x1FFFF000u); r != c.resolved.end()) RetireResolved(r);
  auto it = c.by_base.find(physical_address & 0x1FFFF000u);
  if (it == c.by_base.end()) return;
  for (uint64_t key : it->second) {
    auto t = c.textures.find(key);
    if (t != c.textures.end()) Retire(t);
  }
  c.by_base.erase(it);
}

Image* ResolveTexture(uint32_t physical_address, uint32_t width, uint32_t height, VkFormat format,
                      const uint32_t dest_words[6]) {
  std::lock_guard lock(c.mutex);
  DropWritten();
  const uint32_t address = physical_address & 0x1FFFF000u;
  xenos::xe_gpu_texture_fetch_t dest;
  std::memcpy(&dest, dest_words, sizeof(dest));
  const uint32_t dest_swizzle = dest.swizzle;
  // a texture loaded from here earlier is what the CPU put there before the GPU wrote this over it
  if (auto loaded = c.by_base.find(address); loaded != c.by_base.end()) {
    for (uint64_t key : loaded->second)
      if (auto t = c.textures.find(key); t != c.textures.end()) Retire(t);
    c.by_base.erase(loaded);
  }
  const auto watch = [&](Texture& t) {
    t.guest_w = dest.size_2d.width + 1, t.guest_h = dest.size_2d.height + 1, t.guest_format = uint32_t(dest.format);
    // the CPU writing here later means the game has put something else in this memory
    if (GuestRange(dest, t.base, t.size) && c.memory)
      c.memory->EnablePhysicalMemoryAccessCallbacks(t.base, t.size, true, false);
  };
  if (auto it = c.resolved.find(address); it != c.resolved.end()) {
    Texture& slot = *it->second;
    if (slot.image.width == width && slot.image.height == height && slot.image.format == format) {
      slot.dest_swizzle = dest_swizzle;
      watch(slot);
      return &slot.image;
    }
    RetireResolved(it);
  }
  auto& slot = c.resolved[address];
  REXLOG_INFO("[native] resolve target {:08X} {}x{} format {}", physical_address, width, height, int(format));
  slot = std::make_unique<Texture>();
  if (!MakeImage(slot->image, width, height, format,
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
    slot.reset();
    return nullptr;
  }
  slot->index = HeapAdd(Heap::k2D, slot->image.view);
  slot->dest_swizzle = dest_swizzle;
  watch(*slot);
  return &slot->image;
}

Image* ResolvedImage(uint32_t physical_address) {
  std::lock_guard lock(c.mutex);
  auto it = c.resolved.find(physical_address & 0x1FFFF000u);
  return it == c.resolved.end() ? nullptr : &it->second->image;
}

}  // namespace bardown::native

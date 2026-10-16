// the game's Direct3D draws, drawn natively. the game's own D3D still runs underneath and still feeds the
// emulated GPU; what's on screen is this. only the main device for now: the menus draw on it alone, while a
// match draws mostly on the worker devices, which come next

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/ppc/func.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "guest.h"
#include "native.h"
#include "pipelines.h"
#include "render.h"
#include "streams.h"
#include "textures.h"
#include "vk.h"

REX_EXTERN(__imp__sub_827E5BA0);  // DrawIndexedVertices
REX_EXTERN(__imp__sub_827E5788);  // DrawVertices
REX_EXTERN(__imp__sub_827E52B8);  // BeginVertices
REX_EXTERN(__imp__sub_827E5778);  // EndVertices: moves the ring pointer to where BeginVertices left it
REX_EXTERN(__imp__sub_827DFB60);  // ClearF
REX_EXTERN(__imp__sub_827E6760);  // Resolve
REX_EXTERN(__imp__sub_827E2148);  // resource flush: the CPU wrote a resource's memory
REX_EXTERN(__imp__sub_827E0038);  // segment begin, on a recording device
REX_EXTERN(__imp__sub_827E05A8);  // segment end
REX_EXTERN(__imp__sub_827E1200);  // the main device runs a recorded segment
REX_EXTERN(__imp__sub_827EB308);  // BeginTiling
REX_EXTERN(__imp__sub_827E18B0);  // link a recorded segment

namespace bardown::native {

namespace {

namespace g = guest;

// where the translated vertex shaders expect each input (XenosRecomp's table): usage, usage index, location
constexpr uint8_t kLocations[][3] = {{0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},  {5, 1, 5},
                                     {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},  {1, 0, 10}, {10, 1, 11},
                                     {5, 4, 12}, {5, 5, 13}, {5, 6, 14}, {5, 7, 15}, {0, 1, 15}};
int Location(uint32_t usage, uint32_t index) {
  const auto* l = std::find_if(std::begin(kLocations), std::end(kLocations),
                               [&](const uint8_t* e) { return e[0] == usage && e[1] == index; });
  return l == std::end(kLocations) ? -1 : (*l)[2];
}

constexpr uint32_t kSpecR11G11B10 = 1, kSpecAlphaTest = 2;

// a D3DDECLTYPE as a Vulkan vertex format. the GPU reads vertices a word at a time, swapped by the fetch's
// endian mode, and takes components from the low bits up; once a stream is swapped the same way, Vulkan's
// packed formats read the same components. bits 10-21 of the type are D3D's swizzle on top: XYZW (and the
// 0/1 fill for short types, which Vulkan does too) for everything but D3DCOLOR, which is ZYXW
struct VertexFormat {
  VkFormat format = VK_FORMAT_UNDEFINED;
  bool r11g11b10 = false;
};
bool DeclFormat(uint32_t type, bool integer, VertexFormat& out) {
  const uint32_t fmt = type & 0x3F;
  const bool sign = type & 0x100, unnormalized = type & 0x200;
  const auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uscaled, VkFormat sscaled, VkFormat uint_,
                        VkFormat sint) {
    if (integer) return sign ? sint : uint_;
    if (unnormalized) return sign ? sscaled : uscaled;
    return sign ? snorm : unorm;
  };
  out = {VK_FORMAT_UNDEFINED, false};
  switch (fmt) {
    case 0x06:  // 8_8_8_8
      if ((type >> 10 & 0xFFF) == 0x60A) {
        out = {integer ? VK_FORMAT_B8G8R8A8_UINT : VK_FORMAT_B8G8R8A8_UNORM, false};
      } else {
        out = {pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED,
                    VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT),
               false};
      }
      return true;
    case 0x07:  // 2_10_10_10
      out = {pick(VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                  VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
                  VK_FORMAT_A2B10G10R10_UINT_PACK32, VK_FORMAT_A2B10G10R10_SINT_PACK32),
             false};
      return true;
    case 0x10:  // 10_11_11 and 11_11_10: the shaders unpack these themselves
    case 0x11:
      if (!integer) return false;
      out = {VK_FORMAT_R32_UINT, true};
      return true;
    case 0x19:  // 16_16
      out = {pick(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED,
                  VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT),
             false};
      return true;
    case 0x1A:  // 16_16_16_16
      out = {pick(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_USCALED,
                  VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_SINT),
             false};
      return true;
    case 0x1F:
      out = {integer ? VK_FORMAT_R16G16_UINT : VK_FORMAT_R16G16_SFLOAT, false};
      return true;
    case 0x20:
      out = {integer ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_SFLOAT, false};
      return true;
    case 0x21:
    case 0x24:
      out = {integer ? (sign ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT) : VK_FORMAT_R32_SFLOAT, false};
      return true;
    case 0x22:
    case 0x25:
      out = {integer ? (sign ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT) : VK_FORMAT_R32G32_SFLOAT, false};
      return true;
    case 0x39:
      out = {integer ? VK_FORMAT_R32G32B32_UINT : VK_FORMAT_R32G32B32_SFLOAT, false};
      return true;
    case 0x23:
    case 0x26:
      out = {integer ? (sign ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT)
                     : VK_FORMAT_R32G32B32A32_SFLOAT,
             false};
      return true;
    default:
      return false;
  }
}

// a stream's bytes in host order, by the fetch's endian mode (0 none, 1 8in16, 2 8in32, 3 16in32)
void ConvertVertices(uint8_t* dst, const uint8_t* src, size_t bytes, uint32_t endian) {
  const size_t words = bytes / 4;
  for (size_t i = 0; i < words; ++i) {
    uint32_t x;
    std::memcpy(&x, src + 4 * i, 4);
    switch (endian) {
      case 1:
        x = (x & 0xFF00FF00u) >> 8 | (x & 0x00FF00FFu) << 8;
        break;
      case 2:
        x = __builtin_bswap32(x);
        break;
      case 3:
        x = x >> 16 | x << 16;
        break;
      default:
        break;
    }
    std::memcpy(dst + 4 * i, &x, 4);
  }
}

void SwapWords(uint8_t* dst, const uint8_t* src, size_t words) { ConvertVertices(dst, src, words * 4, 2); }

// a 16-bit element's two halves come out of the word swap the wrong way round (measured in RenderDoc: half
// float positions read (y x w z) with w ~0.07 until swapped back). halves = the vertex's dwords that hold one
void SwapHalves(uint8_t* data, size_t bytes, uint32_t stride, uint64_t halves) {
  if (!halves || stride < 4) return;
  for (size_t v = 0; v + stride <= bytes; v += stride)
    for (uint32_t d = 0; d < stride / 4 && d < 64; ++d) {
      if (!(halves >> d & 1)) continue;
      uint32_t x;
      std::memcpy(&x, data + v + 4 * d, 4);
      x = x >> 16 | x << 16;
      std::memcpy(data + v + 4 * d, &x, 4);
    }
}


// render targets: EDRAM surfaces, kept as native images by base, format and pitch
struct Target {
  Image image;
};
struct Targets {
  std::mutex mutex;  // workers find their targets too
  std::unordered_map<uint64_t, std::unique_ptr<Target>> by_key;
  // the open pass, main thread only. the handles as well as the slots: a target can be remade in place
  Image* color = nullptr;
  Image* depth = nullptr;
  VkImage color_image = VK_NULL_HANDLE, depth_image = VK_NULL_HANDLE;
  bool open = false;
};
Targets t;

VkFormat ColorFormat(uint32_t format) {
  switch (format) {
    case 0:  // 8_8_8_8
    case 1:  // 8_8_8_8_GAMMA
      return VK_FORMAT_R8G8B8A8_UNORM;
    case 2:  // 2_10_10_10
    case 10:
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 4:
      return VK_FORMAT_R16G16_SNORM;
    case 5:
      return VK_FORMAT_R16G16B16A16_SNORM;
    case 6:
      return VK_FORMAT_R16G16_SFLOAT;
    case 14:
      return VK_FORMAT_R32_SFLOAT;
    case 15:
      return VK_FORMAT_R32G32_SFLOAT;
    default:  // the float 2_10_10_10 ones and 16_16_16_16_FLOAT
      return VK_FORMAT_R16G16B16A16_SFLOAT;
  }
}

Image* TargetImage(uint32_t base, uint32_t format, uint32_t pitch, uint32_t height, bool depth) {
  const uint64_t key = uint64_t(base) | uint64_t(format) << 12 | uint64_t(pitch) << 16 | uint64_t(depth) << 40;
  std::lock_guard lock(t.mutex);
  auto& slot = t.by_key[key];
  const uint32_t scale = RenderScale();
  height = std::max(height, 16u);
  if (slot && slot->image.height >= height * scale) return &slot->image;
  if (slot) {
    // taller than first seen: a new image in the same place (content is lost, which a game redrawing every frame
    // doesn't notice). the old one may be in the open pass and in frames still in flight; the pass notices the
    // new handle and starts over
    RetireImage(slot->image);
  } else {
    slot = std::make_unique<Target>();
  }
  const VkFormat vk_format = depth ? DepthFormat() : ColorFormat(format);
  const VkImageUsageFlags usage =
      (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (!MakeImage(slot->image, pitch * scale, height * scale, vk_format, usage)) {
    slot->image = {};
    return nullptr;
  }
  REXLOG_INFO("[native] render target {:X} {}x{} format {}{}", base, pitch, height, format, depth ? " (depth)" : "");
  return &slot->image;
}

void EndPass() {
  if (!t.open) return;
  Api().vkCmdEndRendering(SceneCommands());
  t.open = false;
}

void BeginPass(Image* color, Image* depth) {
  const VkImage color_image = color ? color->image : VK_NULL_HANDLE, depth_image = depth ? depth->image : VK_NULL_HANDLE;
  if (t.open && t.color == color && t.depth == depth && t.color_image == color_image && t.depth_image == depth_image)
    return;
  EndPass();
  t.color = color;
  t.depth = depth;
  t.color_image = color_image;
  t.depth_image = depth_image;
  if (!color && !depth) return;
  VkCommandBuffer cb = SceneCommands();
  if (color) Transition(cb, *color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  if (depth) Transition(cb, *depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo c = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingAttachmentInfo d = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (color) {
    c.imageView = color->view;
    c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  if (depth) {
    d.imageView = depth->view;
    d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  }
  const Image* any = color ? color : depth;
  if (!any) return;
  uint32_t width = any->width, height = any->height;
  if (color && depth) {
    width = std::min(color->width, depth->width);
    height = std::min(color->height, depth->height);
  }
  VkRenderingInfo info = {VK_STRUCTURE_TYPE_RENDERING_INFO};
  info.renderArea = {{0, 0}, {width, height}};
  info.layerCount = 1;
  info.colorAttachmentCount = color ? 1 : 0;
  info.pColorAttachments = &c;
  info.pDepthAttachment = depth ? &d : nullptr;
  info.pStencilAttachment = depth ? &d : nullptr;
  Api().vkCmdBeginRendering(cb, &info);
  t.open = true;
}

// predicated tiling: the 360 drew a frame too big for EDRAM once per tile (two 640x720 halves in a match),
// resolving each. there's no EDRAM here, so a tiled target is one image the size of all the tiles together.
// the device keeps the tile rects at +13128 (x1 y1 x2 y2 each) and their count at +13124 (both left there
// after), and sets bit 5 of the byte at +11068 between BeginTiling and EndTiling: on the main device, which
// resolves the tiles itself in there, and on the worker devices, which get a copy when a segment begins
bool TiledExtent(const uint8_t* base, uint32_t dev, uint32_t& width, uint32_t& height) {
  const uint32_t tiles = std::min(g::U32(base, dev + 13124), 15u);
  if (!tiles || !(g::Byte(base, dev + 11068) & 0x20)) return false;
  width = height = 0;
  for (uint32_t i = 0; i < tiles; ++i) {
    width = std::max(width, g::U32(base, dev + 13128 + 16 * i + 8));
    height = std::max(height, g::U32(base, dev + 13128 + 16 * i + 12));
  }
  return width && height;
}

// the device's current targets, as native images
void CurrentTargets(const uint8_t* base, uint32_t dev, Image*& color, Image*& depth) {
  const uint32_t surface = g::Reg(base, dev, 0x2000), color_info = g::Reg(base, dev, 0x2001),
                 depth_info = g::Reg(base, dev, 0x2002);
  uint32_t pitch = surface & 0x3FFF;
  const uint32_t br = g::Reg(base, dev, 0x2082);
  uint32_t height = (br >> 16) & 0x7FFF;
  TiledExtent(base, dev, pitch, height);
  color = g::U32(base, dev + g::kTargets) ? TargetImage(color_info & 0xFFF, (color_info >> 16) & 0xF, pitch, height, false)
                                         : nullptr;
  depth = g::U32(base, dev + g::kDepth) ? TargetImage(depth_info & 0xFFF, (depth_info >> 16) & 1, pitch, height, true)
                                       : nullptr;
}

struct Rect {
  int32_t x0, y0, x1, y1;
};

Rect Scissor(const uint8_t* base, uint32_t dev, const Image& target) {
  const uint32_t tl = g::Reg(base, dev, 0x2081), br = g::Reg(base, dev, 0x2082);
  const uint32_t stl = g::Reg(base, dev, 0x200E), sbr = g::Reg(base, dev, 0x200F);
  const int32_t s = int32_t(RenderScale());
  Rect r = {int32_t(tl & 0x7FFF), int32_t(tl >> 16 & 0x7FFF), int32_t(br & 0x7FFF), int32_t(br >> 16 & 0x7FFF)};
  if (sbr) {
    r.x0 = std::max(r.x0, int32_t(stl & 0x7FFF));
    r.y0 = std::max(r.y0, int32_t(stl >> 16 & 0x7FFF));
    r.x1 = std::min(r.x1, int32_t(sbr & 0x7FFF));
    r.y1 = std::min(r.y1, int32_t(sbr >> 16 & 0x7FFF));
  }
  r = {r.x0 * s, r.y0 * s, r.x1 * s, r.y1 * s};
  r.x1 = std::min(r.x1, int32_t(target.width));
  r.y1 = std::min(r.y1, int32_t(target.height));
  if (r.x1 < r.x0) r.x1 = r.x0;
  if (r.y1 < r.y0) r.y1 = r.y0;
  return r;
}

float RegF(const uint8_t* base, uint32_t dev, uint32_t reg) {
  const uint32_t v = g::Reg(base, dev, reg);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

VkViewport Viewport(const uint8_t* base, uint32_t dev) {
  const float s = float(RenderScale());
  const float sx = RegF(base, dev, 0x210F), ox = RegF(base, dev, 0x2110), sy = RegF(base, dev, 0x2111),
              oy = RegF(base, dev, 0x2112), sz = RegF(base, dev, 0x2113), oz = RegF(base, dev, 0x2114);
  // the translated vertex shaders already turn y over for Vulkan (built with -fvk-invert-y), so the game's
  // y scale goes in with its sign flipped
  return {(ox - sx) * s, (oy + sy) * s, 2 * sx * s, -2 * sy * s, oz, oz + sz};
}

// a draw whose vertices the game writes after the call (BeginVertices): converted at the device's next call
struct PendingInline {
  uint8_t* dst = nullptr;
  const uint8_t* src = nullptr;
  size_t bytes = 0;
  uint32_t endian = 2, stride = 0;
  uint64_t halves = 0;
};
std::mutex pending_mutex;
std::unordered_map<uint32_t, PendingInline> pending;

void FlushAllInline() {
  std::vector<PendingInline> all;
  {
    std::lock_guard lock(pending_mutex);
    for (auto& [dev, p] : pending) all.push_back(p);
    pending.clear();
  }
  for (const PendingInline& p : all)
    if (p.dst) {
      ConvertVertices(p.dst, p.src, p.bytes, p.endian);
      SwapHalves(p.dst, p.bytes, p.stride, p.halves);
    }
}

void FlushInline(uint32_t dev) {
  PendingInline p;
  {
    std::lock_guard lock(pending_mutex);
    auto it = pending.find(dev);
    if (it == pending.end()) return;
    p = it->second;
    pending.erase(it);
  }
  if (p.dst) {
    ConvertVertices(p.dst, p.src, p.bytes, p.endian);
    SwapHalves(p.dst, p.bytes, p.stride, p.halves);
  }
}

// everything that decides a converted stream's bytes
struct StreamId {
  uint32_t address = 0, endian = 0, stride = 0, wrap_to = 0;
  uint64_t bytes = 0, halves = 0;
  bool operator==(const StreamId&) const = default;
};
struct StreamIdHash {
  size_t operator()(const StreamId& id) const { return XXH3_64bits(&id, sizeof(id)); }
};

// vertex streams converted this frame, shared by every draw reading the same range
struct StreamCopies {
  std::mutex mutex;
  uint64_t frame = 0;
  std::unordered_map<StreamId, Upload, StreamIdHash> by_range;
};
StreamCopies stream_copies;

// wrap_to: when the indices run past the stream (instanced meshes like the crowd: the shader works out each
// vertex's place in its instance from the index and fetches that one), the stream is repeated up to that many
// vertices, so index i reads vertex i mod n as the shader's own fetch would
Upload StreamCopy(uint32_t address, size_t bytes, uint32_t endian, uint32_t stride, uint64_t halves,
                  uint32_t wrap_to) {
  const uint64_t frame = FrameNumber();
  const StreamId id{address, endian, stride, wrap_to, bytes, halves};
  const uint64_t key = StreamIdHash{}(id);
  {
    std::lock_guard lock(stream_copies.mutex);
    if (stream_copies.frame != frame) {
      stream_copies.by_range.clear();
      stream_copies.frame = frame;
    }
    if (auto it = stream_copies.by_range.find(id); it != stream_copies.by_range.end()) return it->second;
  }
  const size_t total = stride && wrap_to ? std::max(bytes, size_t(wrap_to) * stride) : bytes;
  auto convert = [&](uint8_t* dst) {
    ConvertVertices(dst, Physical(address), bytes, endian);
    SwapHalves(dst, bytes, stride, halves);
    const size_t whole = stride ? bytes / stride * stride : bytes;
    for (size_t at = whole; whole && at < total; at += whole) std::memcpy(dst + at, dst, std::min(whole, total - at));
  };
  Upload u;
  bool kept;
  {
    auto lock = StreamLock();
    uint8_t* fill;
    kept = FindStream(key, address, Physical(address), bytes, total, u, fill);
    if (kept && fill) convert(fill);
  }
  if (!kept) {
    u = Allocate(total, 16);
    convert(u.host);
  }
  std::lock_guard lock(stream_copies.mutex);
  if (stream_copies.frame == frame) stream_copies.by_range.emplace(id, u);
  return u;
}

enum class Kind { kIndexed, kAuto, kInline };
struct DrawCall {
  Kind kind = Kind::kAuto;
  uint32_t prim = 0, base_vertex = 0, start = 0, count = 0, inline_stride = 0;
  uint32_t inline_data = 0;  // where the game writes an inline draw's vertices
};

struct Stats {
  std::atomic<uint64_t> draws{0}, skipped{0};
  uint64_t missed_segments = 0;
  double assemble_ms = 0, wait_ms = 0;
  uint64_t frame = 0;
  uint64_t trace = [] {
    const char* v = std::getenv("BARDOWN_NATIVE_TRACE");
    return v ? std::strtoull(v, nullptr, 10) : UINT64_MAX;
  }();
  // BARDOWN_NATIVE_TRACE_FRAMES: how many frames from that one (default 1)
  uint64_t trace_frames = [] {
    const char* v = std::getenv("BARDOWN_NATIVE_TRACE_FRAMES");
    return v ? std::max<uint64_t>(1, std::strtoull(v, nullptr, 10)) : 1;
  }();
  bool tracing() const { return trace != UINT64_MAX && frame >= trace && frame < trace + trace_frames; }
  // the replay trace needs every draw described, whichever frame it was recorded in
  bool describing() const { return trace != UINT64_MAX; }
};
Stats stats;
std::atomic<int> segment_log{0};

void Skip(const char* why) {
  ++stats.skipped;
  static std::mutex mutex;
  static std::unordered_map<const char*, bool> told;
  std::lock_guard lock(mutex);
  if (!told[why]) REXLOG_WARN("[native] skipping draws: {}", why);
  told[why] = true;
}

// a draw or clear, ready to go into the frame: recorded on whichever thread the game drew from, replayed
// on the main thread in the game's order
struct Record {
  enum class Op : uint8_t { kDraw, kClear, kResolve } op = Op::kDraw;
  Image* color = nullptr;
  Image* depth = nullptr;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDeviceAddress push[3] = {};
  VkViewport viewport = {};
  VkRect2D scissor = {};
  float blend[4] = {};
  uint32_t stencil_front = 0, stencil_back = 0;
  uint32_t binding_count = 0;
  VkBuffer buffers[16] = {};
  VkDeviceSize offsets[16] = {};
  VkBuffer index_buffer = VK_NULL_HANDLE;
  VkDeviceSize index_offset = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  bool indexed = false;
  uint32_t count = 0, first = 0;
  int32_t vertex_offset = 0;
  uint32_t clears = 0;
  VkClearAttachment attachments[2] = {};
  VkClearRect rect = {};
  // the constants as uploaded, still on the host side until the frame is submitted
  // VS then PS, 4 KB each, in ordinary memory: merged with the GPU's at assembly, then written out to `upload`
  // (the upload space is write-combined, far too slow to read back)
  std::shared_ptr<uint8_t[]> consts;
  uint8_t* upload = nullptr;
  // the constant blocks this draw uploads (the device's pending bits as it draws); the rest are the GPU's
  uint64_t vs_dirty = 0, ps_dirty = 0;
  uint32_t ib_address = 0;  // for BARDOWN_NATIVE_IB
  std::string trace;        // BARDOWN_NATIVE_TRACE: what this draw is, logged as it's replayed
  std::vector<uint64_t> late;  // late textures it reads
  // a resolve: color is the source, this the destination
  Image* resolve_to = nullptr;
  VkImageCopy copy = {};
};

// the GPU's constant registers as the spliced stream leaves them, main thread only. a worker segment only sets
// what it doesn't inherit; the rest is whatever the main device and earlier segments left behind
struct GpuConstants {
  uint8_t vs[4096] = {}, ps[4096] = {};
};
GpuConstants gpu;

// pending/inherit masks: one bit per 4 registers (64 bytes), register 0 in the top bit
void MergeConstants(uint8_t* gpu_file, uint8_t* draw, uint64_t inherit) {
  for (uint32_t k = 0; k < 64; ++k) {
    if (inherit >> (63 - k) & 1)
      std::memcpy(draw + 64 * k, gpu_file + 64 * k, 64);
    else
      std::memcpy(gpu_file + 64 * k, draw + 64 * k, 64);
  }
}

void Replay(const Record& r) {
  if (r.op == Record::Op::kResolve) {
    EndPass();
    VkCommandBuffer cb = SceneCommands();
    Transition(cb, *r.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cb, *r.resolve_to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    Api().vkCmdCopyImage(cb, r.color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.resolve_to->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r.copy);
    Transition(cb, *r.resolve_to, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return;
  }
  if (stats.tracing()) {
    static int replayed = 0;
    static uint64_t replay_frame = 0;
    if (replay_frame != stats.frame) {
      replay_frame = stats.frame;
      replayed = 0;
    }
    REXLOG_INFO("[replay] f{} {} {} consts h{:016X} vp {} {} {} {} {} {} sc {} {} {} {}", stats.frame, replayed++, r.trace,
                r.upload ? XXH3_64bits(r.upload, 8192) : 0, r.viewport.x, r.viewport.y, r.viewport.width,
                r.viewport.height, r.viewport.minDepth, r.viewport.maxDepth, r.scissor.offset.x, r.scissor.offset.y,
                r.scissor.extent.width, r.scissor.extent.height);
  }
  for (uint64_t key : r.late) RefreshLate(key);
  BeginPass(r.color, r.depth);
  VkCommandBuffer cb = SceneCommands();
  auto& v = Api();
  if (r.op == Record::Op::kClear) {
    v.vkCmdClearAttachments(cb, r.clears, r.attachments, 1, &r.rect);
    return;
  }
  v.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
  BindHeaps(cb);
  v.vkCmdPushConstants(cb, ShaderLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(r.push), r.push);
  v.vkCmdSetViewport(cb, 0, 1, &r.viewport);
  v.vkCmdSetScissor(cb, 0, 1, &r.scissor);
  v.vkCmdSetBlendConstants(cb, r.blend);
  // RB_STENCILREFMASK: reference, compare mask, write mask a byte each; _BF the same for back faces
  v.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, r.stencil_front & 0xFF);
  v.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, r.stencil_front >> 8 & 0xFF);
  v.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, r.stencil_front >> 16 & 0xFF);
  v.vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, r.stencil_back & 0xFF);
  v.vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, r.stencil_back >> 8 & 0xFF);
  v.vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, r.stencil_back >> 16 & 0xFF);
  if (r.binding_count) v.vkCmdBindVertexBuffers(cb, 0, r.binding_count, r.buffers, r.offsets);
  if (r.indexed) {
    v.vkCmdBindIndexBuffer(cb, r.index_buffer, r.index_offset, r.index_type);
    v.vkCmdDrawIndexed(cb, r.count, 1, 0, r.vertex_offset, 0);
  } else {
    v.vkCmdDraw(cb, r.count, 1, r.first, 0);
  }
}

// the frame as the GPU would run it. the main device's draws, clears and resolves go in as they come; where it
// runs a worker's segment (827E1200) it only leaves a slot, because the worker records the segment afterwards
// and links it into that slot (827E18B0), and the GPU waits there until it has. so the frame is put together
// at the swap, slots filled
struct Slot {
  uint32_t link = 0;
  uint64_t vs_inherit = 0, ps_inherit = 0, vs_dirty = 0, ps_dirty = 0;
  std::vector<uint8_t> main_consts;  // the main device's VS then PS constants, which it flushes first
};
struct Entry {
  bool is_slot = false;
  Record record;
  Slot slot;
};
std::vector<Entry> frame_entries;  // main thread only

struct Segments {
  std::mutex mutex;
  std::condition_variable linked_cv;
  std::unordered_map<uint32_t, std::vector<Record>> recording;  // by worker device
  struct Linked {
    std::vector<Record> records;
    uint64_t frame = 0;
  };
  std::unordered_map<uint32_t, Linked> linked;  // by the link object the main device ran
  uint64_t overwritten = 0;  // linked again before the frame with the slot used the first one
};
Segments segments;
// the segment this worker thread finished last, until it links it
thread_local std::vector<Record> finished_segment;
thread_local bool has_finished_segment = false;

// draws, clears and resolves the native renderer has taken aren't passed on to the emulated GPU at all.
// BARDOWN_NATIVE_SHADOW=1 passes them on anyway, so both pictures are made (for comparing the two)
bool NativeOnly() {
  static const bool on = std::getenv("BARDOWN_NATIVE_SHADOW") == nullptr;
  return on;
}

// the team-colour composition the game draws at load is resolved to memory and read back by the CPU (it
// compresses the result into the gear textures), so the emulated GPU draws it too, or without one it's written
// back natively at the resolve. from a composition draw
// until the resolve that copies the target out, everything on that target goes both ways
struct Composing {
  std::mutex mutex;
  std::unordered_map<uint32_t, uint32_t> target;  // device -> colour target (RB_COLOR_INFO)
};
Composing composing;

bool AlsoEmulated(const uint8_t* base, uint32_t dev, bool draw) {
  const uint32_t color_info = g::Reg(base, dev, 0x2001);
  std::lock_guard lock(composing.mutex);
  if (draw && CompositionShader(GuestShaderHash(g::U32(base, dev + g::kPixelShader)))) {
    composing.target[dev] = color_info;
    return true;
  }
  auto it = composing.target.find(dev);
  return it != composing.target.end() && it->second == color_info;
}

void ResolvedOut(uint32_t dev) {
  std::lock_guard lock(composing.mutex);
  composing.target.erase(dev);
}

// where a device's draws go: the frame's stream (the main device), its segment (a worker), or nowhere
enum class Route { kNone, kNow, kSegment };
Route RouteFor(uint32_t dev) {
  if (!Drawing()) return Route::kNone;
  switch (DeviceType(dev)) {
    case 1:
      return Route::kNow;
    case 2: {
      std::lock_guard lock(segments.mutex);
      return segments.recording.count(dev) ? Route::kSegment : Route::kNone;
    }
    default:
      return Route::kNone;
  }
}

void Submit(uint32_t dev, Route route, const Record& r) {
  if (route == Route::kNow) {
    frame_entries.push_back({false, r, {}});
    return;
  }
  std::lock_guard lock(segments.mutex);
  auto it = segments.recording.find(dev);
  if (it != segments.recording.end()) it->second.push_back(r);
}

// puts the frame together in the GPU's order and records it
// BARDOWN_NATIVE_DUMP=<frame>: every indexed draw's constants for 5 frames, as they go out, named by index buffer
// and count, to set against the emulated GPU's for the same draws (BARDOWN_TRACE_SWAPS in the SDK)
void DumpConstants(const Record& r, const char* who) {
  static const long from = std::getenv("BARDOWN_NATIVE_DUMP") ? std::atol(std::getenv("BARDOWN_NATIVE_DUMP")) : -1;
  static int n = 0;
  if (from < 0 || !r.ib_address || !r.consts || long(stats.frame) < from || long(stats.frame) >= from + 5) return;
  if (FILE* f = std::fopen(fmt::format("logs/native/ib/native_{:08X}_{}_{:05}_{}.bin", r.ib_address, r.count, n++, who)
                               .c_str(),
                           "wb")) {
    std::fwrite(r.consts.get(), 1, 8192, f);
    std::fclose(f);
  }
}

void Assemble() {
  using namespace std::chrono_literals;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + 100ms;
  for (Entry& e : frame_entries) {
    if (!e.is_slot) {
      // the main device's shadow is what the GPU holds after its draws
      if (e.record.consts) {
        std::memcpy(gpu.vs, e.record.consts.get(), sizeof(gpu.vs));
        std::memcpy(gpu.ps, e.record.consts.get() + 4096, sizeof(gpu.ps));
        std::memcpy(e.record.upload, e.record.consts.get(), 8192);
      }
      DumpConstants(e.record, "main");
      Replay(e.record);
      continue;
    }
    std::vector<Record> records;
    {
      std::unique_lock lock(segments.mutex);
      const auto waited = std::chrono::steady_clock::now();
      segments.linked_cv.wait_until(lock, deadline, [&] { return segments.linked.count(e.slot.link) != 0; });
      stats.wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waited).count();
      auto it = segments.linked.find(e.slot.link);
      if (it == segments.linked.end()) {
        ++stats.missed_segments;
        continue;
      }
      records.swap(it->second.records);
      segments.linked.erase(it);
    }
    Slot& sl = e.slot;
    MergeConstants(gpu.vs, sl.main_consts.data(), ~sl.vs_dirty);
    MergeConstants(gpu.ps, sl.main_consts.data() + 4096, ~sl.ps_dirty);
    for (Record& r : records) {
      // inherited blocks come from the GPU, unless the worker has set them itself since
      if (r.consts) {
        MergeConstants(gpu.vs, r.consts.get(), sl.vs_inherit & ~r.vs_dirty);
        MergeConstants(gpu.ps, r.consts.get() + 4096, sl.ps_inherit & ~r.ps_dirty);
        std::memcpy(r.upload, r.consts.get(), 8192);
        DumpConstants(r, "work");
      }
      Replay(r);
    }
  }
  frame_entries.clear();
  EndPass();
  stats.assemble_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
}

void Draw(uint8_t* base, uint32_t dev, Route route, const DrawCall& call) {
  const ShaderInfo* vs = Shader(GuestShaderHash(g::U32(base, dev + g::kVertexShader)));
  const uint64_t ps_hash = GuestShaderHash(g::U32(base, dev + g::kPixelShader));
  const ShaderInfo* ps = ps_hash ? Shader(ps_hash) : nullptr;
  if (!vs || (ps_hash && !ps)) return Skip("a shader isn't translated");

  Image *color, *depth;
  CurrentTargets(base, dev, color, depth);
  if (!color && !depth) return Skip("no render target");
  const uint32_t depthctl = g::Reg(base, dev, 0x2200);
  if (depth && !(depthctl & 7)) depth = nullptr;  // no depth or stencil test: leave it out of the pass

  PipelineKey key;
  key.vs = GuestShaderHash(g::U32(base, dev + g::kVertexShader));
  key.ps = ps_hash;
  key.color_format = color ? color->format : VK_FORMAT_UNDEFINED;
  key.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
  key.depth = depth ? depthctl : 0;
  key.color_mask = g::Reg(base, dev, 0x2104) & 0xF;
  const uint32_t blend = g::Reg(base, dev, 0x2201);
  // one, zero, add is no blending at all
  key.blend = (blend & 0x1FFF1FFF) == 0x00010001 ? 0 : blend;
  const uint32_t mode = g::Reg(base, dev, 0x2205);
  key.cull = (mode & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (mode & 2 ? VK_CULL_MODE_BACK_BIT : 0);
  key.front_cw = (mode >> 2) & 1;
  const uint32_t colorctl = g::Reg(base, dev, 0x2202);
  // alpha to mask (bit 4) splits a pixel's coverage by alpha across the 360's samples; with one sample here that
  // comes to keeping the texels at least half opaque. nets and fences use it, and drew solid without it
  const bool alpha_to_mask = (colorctl & 0x10) && !(colorctl & 8);
  if ((colorctl & 8) || alpha_to_mask) key.spec |= kSpecAlphaTest;

  // vertices
  struct Stream {
    uint32_t address = 0, size = 0, stride = 0, endian = 2;
    uint64_t halves = 0;
    int binding = -1;
  };
  std::array<Stream, 16> streams;
  uint32_t provided = 0;
  // for fetches from computed indices (vfetchIndexed): per input location, the stream, offset and type
  struct Fetchable {
    int binding = -1;
    uint32_t offset = 0, type = 0;
  };
  std::array<Fetchable, 16> fetchable;
  const uint32_t decl = g::U32(base, dev + g::kDecl);
  const uint32_t decl_count = decl ? std::min(g::U32(base, decl + 24), 16u) : 0;
  for (uint32_t i = 0; i < decl_count; ++i) {
    const uint32_t e = decl + 52 + 12 * i;
    const uint32_t stream = g::U16(base, e), offset = g::U16(base, e + 2), type = g::U32(base, e + 4);
    if (stream >= 16) continue;
    const int loc = Location(g::Byte(base, e + 9), g::Byte(base, e + 10));
    if (loc < 0 || (provided & (1u << loc))) continue;
    // an element the shader only reads through vfetchIndexed isn't an input once compiled, but its stream and
    // its place in the fetch table still are
    const bool attribute = vs->inputs & (1u << loc);
    VertexFormat f;
    if (!DeclFormat(type, vs->integer_inputs & (1u << loc), f)) {
      static uint64_t told = 0;
      if (!(told & (1ull << (type & 0x3F))))
        REXLOG_WARN("[native] vertex format {:06X} isn't handled yet", type);
      told |= 1ull << (type & 0x3F);
      continue;
    }
    if (attribute && f.r11g11b10) key.spec |= kSpecR11G11B10;
    Stream& s = streams[stream];
    if (stats.tracing()) {
      const uint32_t fetch = dev + g::StreamFetch(stream);
      REXLOG_INFO("[trace] element usage {} {} type {:06X} offset {} stream {} endian {} stride {}/{} integer {}",
                  g::Byte(base, e + 9), g::Byte(base, e + 10), type, offset, stream, g::U32(base, fetch + 4) & 3,
                  g::Byte(base, dev + g::kStreamStride + stream) * 4u, g::Byte(base, dev + g::kDeclStride + stream) * 4u,
                  (vs->integer_inputs >> loc) & 1);
    }
    if (s.binding < 0) {
      s.binding = key.binding_count++;
      const uint32_t fetch = dev + g::StreamFetch(stream);
      const uint32_t d0 = g::U32(base, fetch), d1 = g::U32(base, fetch + 4);
      s.address = d0 & 0x1FFFFFFC;
      s.size = (d1 >> 2 & 0xFFFFFF) * 4;
      s.endian = d1 & 3;
      // SetStreamSource's stride, or when it was given 0, the declaration's (both in dwords)
      const uint32_t set = g::Byte(base, dev + g::kStreamStride + stream), declared = g::Byte(base, dev + g::kDeclStride + stream);
      s.stride = call.kind == Kind::kInline ? call.inline_stride : (set ? set : declared) * 4u;
      key.strides[s.binding] = uint16_t(s.stride);
    }
    if (attribute) key.attributes[key.attribute_count++] = {uint8_t(loc), uint8_t(s.binding), uint16_t(offset), f.format};
    if (loc < 16) fetchable[loc] = {s.binding, offset, type};
    const uint32_t fmt = type & 0x3F;
    if (fmt == 0x19 || fmt == 0x1A || fmt == 0x1F || fmt == 0x20) {
      const uint32_t words = (fmt == 0x1A || fmt == 0x20) ? 2 : 1;
      for (uint32_t w = 0; w < words && offset / 4 + w < 64; ++w) s.halves |= 1ull << (offset / 4 + w);
    }
    provided |= 1u << loc;
  }
  // inputs the declaration doesn't feed read zeros
  const uint32_t missing = vs->inputs & ~provided;
  int zero_binding = -1;
  if (missing) {
    zero_binding = key.binding_count++;
    key.strides[zero_binding] = 0;
    for (uint32_t loc = 0; loc < 32 && key.attribute_count < 16; ++loc) {
      if (!(missing & (1u << loc))) continue;
      key.attributes[key.attribute_count++] = {
          uint8_t(loc), uint8_t(zero_binding), 0,
          (vs->integer_inputs & (1u << loc)) ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT};
    }
  }

  // primitives
  bool quads = false;
  switch (call.prim) {
    case 1: key.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: key.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 5: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 13:
      key.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
      quads = true;
      break;
    default:
      return Skip("a primitive type that isn't handled yet (rectangle lists)");
  }
  if (call.kind == Kind::kIndexed && (mode >> 21 & 1) && !quads) key.restart = 1;

  VkPipeline pipeline = Pipeline(key);
  if (!pipeline) return Skip("a pipeline didn't build");

  // how many vertices the indices reach
  uint32_t need_vertices = 0;
  if (call.kind == Kind::kIndexed) {
    if (const uint32_t ib = g::U32(base, dev + g::kIndices)) {
      const bool wide = g::U32(base, ib) >> 31;
      const uint8_t* src = Physical(g::PhysicalAddress(g::U32(base, ib + 24))) + size_t(call.start) * (wide ? 4 : 2);
      uint32_t top = 0;
      for (uint32_t i = 0; i < call.count; ++i) {
        uint32_t v;
        if (wide) {
          std::memcpy(&v, src + 4 * i, 4);
          v = __builtin_bswap32(v);
          if (v == 0xFFFFFFFFu) continue;
        } else {
          uint16_t h;
          std::memcpy(&h, src + 2 * i, 2);
          v = __builtin_bswap16(h);
          if (v == 0xFFFF) continue;
        }
        top = std::max(top, v);
      }
      need_vertices = top + call.base_vertex + 1;
    }
  }

  // the data: vertices converted into this frame's upload space
  VkBuffer buffers[16];
  VkDeviceSize offsets[16];
  VkDeviceAddress addresses[16] = {};
  uint32_t strides[16] = {};
  for (Stream& s : streams) {
    if (s.binding < 0) continue;
    uint32_t count;
    if (call.kind == Kind::kInline)
      count = call.count;
    else
      count = s.stride ? s.size / s.stride : 1;
    count = std::max(count, 1u);
    // no stride on the stream: shaders that index it themselves step by their fetch instruction's stride, so
    // the whole buffer goes up
    const size_t bytes = call.kind != Kind::kInline && !s.stride
                             ? (size_t(s.size) + 3) & ~size_t(3)
                             : (size_t(count) * std::max(s.stride, 4u) + 3) & ~size_t(3);
    Upload u;
    if (call.kind == Kind::kInline) {
      // the game hasn't written them yet; BeginVertices' own fetch is always 8in32
      u = Allocate(bytes, 16);
      FlushInline(dev);
      std::lock_guard lock(pending_mutex);
      pending[dev] = {u.host, g::Host(base, call.inline_data), bytes, 2, s.stride, s.halves};
    } else {
      u = StreamCopy(s.address, std::min<size_t>(bytes, s.size), s.endian, s.stride, s.halves,
                     need_vertices > count ? need_vertices : 0);
    }
    buffers[s.binding] = u.buffer;
    offsets[s.binding] = u.offset;
    addresses[s.binding] = u.address;
    strides[s.binding] = s.stride;
  }
  if (zero_binding >= 0) {
    Upload u = Allocate(16, 16);
    std::memset(u.host, 0, 16);
    buffers[zero_binding] = u.buffer;
    offsets[zero_binding] = u.offset;
  }

  // indices
  Upload indices{};
  uint32_t ib_address = 0;
  VkIndexType index_type = VK_INDEX_TYPE_UINT16;
  uint32_t index_count = call.count;
  if (call.kind == Kind::kIndexed) {
    const uint32_t ib = g::U32(base, dev + g::kIndices);
    if (!ib) return Skip("an indexed draw without indices");
    const bool wide = g::U32(base, ib) >> 31;
    // the header keeps the virtual address, like a texture's
    const uint32_t address = g::PhysicalAddress(g::U32(base, ib + 24));
    ib_address = address;
    const uint8_t* src = Physical(address) + size_t(call.start) * (wide ? 4 : 2);
    if (quads) {
      index_count = call.count / 4 * 6;
      indices = Allocate(size_t(index_count) * 4, 16);
      index_type = VK_INDEX_TYPE_UINT32;
      auto* out = reinterpret_cast<uint32_t*>(indices.host);
      for (uint32_t q = 0; q < call.count / 4; ++q) {
        uint32_t v[4];
        for (int k = 0; k < 4; ++k)
          v[k] = wide ? __builtin_bswap32(*reinterpret_cast<const uint32_t*>(src + 4 * (4 * q + k)))
                      : __builtin_bswap16(*reinterpret_cast<const uint16_t*>(src + 2 * (4 * q + k)));
        const uint32_t tri[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
        std::memcpy(out + 6 * q, tri, sizeof(tri));
      }
    } else {
      index_type = wide ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
      indices = Allocate(size_t(call.count) * (wide ? 4 : 2), 16);
      if (wide) {
        SwapWords(indices.host, src, call.count);
      } else {
        for (uint32_t i = 0; i < call.count; ++i) {
          uint16_t x;
          std::memcpy(&x, src + 2 * i, 2);
          x = __builtin_bswap16(x);
          std::memcpy(indices.host + 2 * i, &x, 2);
        }
      }
    }
  } else if (quads) {
    index_count = call.count / 4 * 6;
    indices = Allocate(size_t(index_count) * 4, 16);
    index_type = VK_INDEX_TYPE_UINT32;
    auto* out = reinterpret_cast<uint32_t*>(indices.host);
    const uint32_t first = call.kind == Kind::kAuto ? call.start : 0;
    for (uint32_t q = 0; q < call.count / 4; ++q) {
      const uint32_t v = first + 4 * q;
      const uint32_t tri[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
      std::memcpy(out + 6 * q, tri, sizeof(tri));
    }
  }

  // constants: the device's shadow, turned to host order
  // the uploaded copy is filled in at assembly, once the inherited ones are known
  Upload consts_upload = Allocate(8192, 256), shared = Allocate(1056, 256);
  std::shared_ptr<uint8_t[]> consts(new uint8_t[8192]);
  SwapWords(consts.get(), g::Host(base, dev + g::kVsConsts), 1024);
  SwapWords(consts.get() + 4096, g::Host(base, dev + g::kPsConsts), 1024);
  std::memset(shared.host, 0, 1056);
  auto* sc = reinterpret_cast<uint32_t*>(shared.host);
  // from 800: each slot's texture read scale, 2^(the fetch constant's exponent bias, dword 3 bits 13-18)
  float tex_scale[32];
  std::fill(std::begin(tex_scale), std::end(tex_scale), 1.0f);
  // from 928: each slot's LOD field (dword 4 bits 12-21, signed, in 1/32 of a level)
  float tex_lod[32] = {};
  std::string slots_trace;
  std::vector<uint64_t> late;
  for (uint32_t slot = 0; slot < 32; ++slot) {
    if (slot < 26 && !g::U32(base, dev + g::kTextures + 4 * slot)) continue;
    uint32_t words[6];
    for (int w = 0; w < 6; ++w) words[w] = g::U32(base, dev + g::kFetch + 24 * slot + 4 * w);
    if ((words[0] & 3) != 2) continue;  // not a texture fetch constant
    if (stats.describing())
      slots_trace += fmt::format(" s{}[{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", slot, words[0], words[1], words[2],
                                 words[3], words[4], words[5]);
    const TextureBinding b = BindTexture(words, slot >= 16);
    if (b.late) late.push_back(b.late);
    tex_scale[slot] = std::ldexp(1.0f, int32_t(words[3] << 13) >> 26);
    tex_lod[slot] = float(int32_t(words[4] << 10) >> 22) / 32.0f;
    sc[int(b.heap) * 32 + slot] = b.index;
    // slots 16 and up are the vertex shaders', which read whole texels (bone matrices) and mustn't blend them
    sc[96 + slot] = slot >= 16 ? PointSampler() : b.sampler;
  }
  const uint32_t bools0 = g::U32(base, dev + g::kBoolConsts), bools4 = g::U32(base, dev + g::kBoolConsts + 16);
  sc[128] = (bools0 & 0xFFFF) | (bools4 & 0xFFFF) << 16;
  // the 360 put pixel centres on whole coordinates; half a pixel over lines them up with Vulkan's
  if (!(g::Reg(base, dev, 0x2302) & 1)) {
    // added before the shader turns y over, so in the game's own clip space
    const float sx = RegF(base, dev, 0x210F), sy = RegF(base, dev, 0x2111);
    const float half[2] = {sx ? 0.5f / sx : 0.0f, sy ? 0.5f / sy : 0.0f};
    std::memcpy(&sc[130], half, sizeof(half));
  }
  const float alpha_ref = alpha_to_mask ? 0.5f : RegF(base, dev, 0x210E);
  std::memcpy(&sc[132], &alpha_ref, 4);
  // the comparison too (RB_COLORCONTROL bits 0-2: 3 keeps alpha at or below the reference, 4 above it, ...); alpha to
  // mask keeps what's at least half opaque
  sc[133] = alpha_to_mask ? 6u : colorctl & 7;
  std::memcpy(shared.host + 800, tex_scale, sizeof(tex_scale));
  std::memcpy(shared.host + 928, tex_lod, sizeof(tex_lod));
  // from 544: the vfetchIndexed table, 16 bytes a location
  for (uint32_t loc = 0; loc < 16; ++loc) {
    const Fetchable& f = fetchable[loc];
    if (f.binding < 0) continue;
    const VkDeviceAddress at = addresses[f.binding] + f.offset;
    const uint32_t entry[4] = {uint32_t(at), uint32_t(at >> 32), strides[f.binding], f.type};
    std::memcpy(shared.host + 544 + 16 * loc, entry, sizeof(entry));
  }

  if (stats.tracing())
    REXLOG_INFO("[trace] draw {} prim {} n {} vs {:016X} ps {:016X} rt {} {}x{} ds {} tex {:X} {:X} {:X} {:X} "
                "depthctl {:08X} sref {:08X} mask {:X} blend {:08X} colorctl {:08X} dev {:08X} tiles {} flag {} "
                "tile0 {} {} {} {} tile1 {} {} {} {} surf {:08X} wo {:08X} sc {:08X} {:08X}",
                int(call.kind), call.prim, call.count, key.vs, key.ps, color ? static_cast<const void*>(color) : static_cast<const void*>(depth),
                (color ? color : depth)->width, (color ? color : depth)->height, depth ? 1 : 0, sc[0], sc[1], sc[2],
                sc[3], depthctl, g::Reg(base, dev, 0x210D), key.color_mask, key.blend, colorctl, dev,
                g::U32(base, dev + 13124), g::Byte(base, dev + 11068), g::U32(base, dev + 13128), g::U32(base, dev + 13132),
                g::U32(base, dev + 13136), g::U32(base, dev + 13140), g::U32(base, dev + 13144),
                g::U32(base, dev + 13148), g::U32(base, dev + 13152), g::U32(base, dev + 13156),
                g::Reg(base, dev, 0x2000), g::Reg(base, dev, 0x2080), g::Reg(base, dev, 0x2081), g::Reg(base, dev, 0x2082));
  ++stats.draws;
  Record r;
  r.late = std::move(late);
  r.color = color;
  r.depth = depth;
  r.pipeline = pipeline;
  r.consts = consts;
  r.upload = consts_upload.host;
  r.ib_address = ib_address;
  if (stats.describing())
  {
    std::string first;
    if (indices.host)
      for (uint32_t i = 0; i < std::min(index_count, 12u); ++i)
        first += fmt::format(" {}", index_type == VK_INDEX_TYPE_UINT32 ? reinterpret_cast<const uint32_t*>(indices.host)[i]
                                                                       : reinterpret_cast<const uint16_t*>(indices.host)[i]);
    for (const Stream& st : streams)
      if (st.binding >= 0)
        first += fmt::format(" stream{} {:08X}+{} h{:04X}", st.binding, st.address, st.size,
                             XXH3_64bits(Physical(st.address), std::min<size_t>(st.size, 1 << 20)) & 0xFFFF);
    r.trace = fmt::format("n {} vs {:016X} ps {:016X} kind {} prim {} base {} start {} rt {} ds {} depthctl {:08X} "
                          "blend {:08X} colorctl {:08X} dev {:08X} idx{}{}",
                          call.count, key.vs, key.ps, int(call.kind), call.prim, call.base_vertex, call.start,
                          static_cast<const void*>(color), static_cast<const void*>(depth), depthctl, key.blend,
                          colorctl, dev,
                          first, slots_trace);
  }
  r.vs_dirty = g::U64(base, dev);
  r.ps_dirty = g::U64(base, dev + 8);
  r.push[0] = consts_upload.address;
  r.push[1] = consts_upload.address + 4096;
  r.push[2] = shared.address;
  r.viewport = Viewport(base, dev);
  const Rect sr = Scissor(base, dev, color ? *color : *depth);
  r.scissor = {{sr.x0, sr.y0}, {uint32_t(sr.x1 - sr.x0), uint32_t(sr.y1 - sr.y0)}};
  for (int i = 0; i < 4; ++i) r.blend[i] = RegF(base, dev, 0x2105 + i);
  r.stencil_front = g::Reg(base, dev, 0x210D);
  r.stencil_back = (depthctl & 0x80) ? g::Reg(base, dev, 0x210C) : r.stencil_front;
  r.binding_count = key.binding_count;
  std::copy(buffers, buffers + key.binding_count, r.buffers);
  std::copy(offsets, offsets + key.binding_count, r.offsets);
  if (indices.host) {
    r.indexed = true;
    r.index_buffer = indices.buffer;
    r.index_offset = indices.offset;
    r.index_type = index_type;
    r.count = index_count;
    r.vertex_offset = int32_t(call.kind == Kind::kIndexed ? call.base_vertex : 0);
  } else {
    r.count = call.count;
    r.first = call.kind == Kind::kAuto ? call.start : 0;
  }
  Submit(dev, route, r);
}

// ClearF: flags (D3DCLEAR_TARGET0-3 = 1-8, ZBUFFER 0x10, STENCIL 0x20), rect or 0, float4 colour, z, stencil
void Clear(const uint8_t* base, uint32_t dev, Route route, uint32_t flags, uint32_t rect, uint32_t color_ptr, float z,
           uint32_t stencil) {
  Image *color, *depth;
  CurrentTargets(base, dev, color, depth);
  Record r;
  r.op = Record::Op::kClear;
  // the pass keeps both targets as they're bound, so it doesn't restart for nothing
  r.color = color;
  r.depth = depth;
  if (!(flags & 0xF)) color = nullptr;
  if (!(flags & 0x30)) depth = nullptr;
  const Image* target = color ? color : depth;
  if (!target) return;
  Rect cr;
  if (rect) {
    const int32_t s = int32_t(RenderScale());
    cr = {int32_t(g::U32(base, rect)) * s, int32_t(g::U32(base, rect + 4)) * s, int32_t(g::U32(base, rect + 8)) * s,
          int32_t(g::U32(base, rect + 12)) * s};
  } else {
    const VkViewport v = Viewport(base, dev);
    cr = {int32_t(std::min(v.x, v.x + v.width)), int32_t(std::min(v.y, v.y + v.height)),
          int32_t(std::max(v.x, v.x + v.width)), int32_t(std::max(v.y, v.y + v.height))};
  }
  cr.x0 = std::max(cr.x0, 0);
  cr.y0 = std::max(cr.y0, 0);
  cr.x1 = std::min(cr.x1, int32_t(target->width));
  cr.y1 = std::min(cr.y1, int32_t(target->height));
  if (cr.x1 <= cr.x0 || cr.y1 <= cr.y0) return;
  if (color) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    a.colorAttachment = 0;
    for (int i = 0; i < 4; ++i) {
      const uint32_t w = color_ptr ? g::U32(base, color_ptr + 4 * i) : 0;
      std::memcpy(&a.clearValue.color.float32[i], &w, 4);
    }
  }
  if (depth) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = ((flags & 0x10) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((flags & 0x20) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    a.colorAttachment = 0;
    a.clearValue.depthStencil = {z, stencil & 0xFF};
  }
  r.rect = {{{cr.x0, cr.y0}, {uint32_t(cr.x1 - cr.x0), uint32_t(cr.y1 - cr.y0)}}, 0, 1};
  Submit(dev, route, r);
}

// clears the device's targets whole, as BeginTiling does tile by tile
void ClearWhole(const uint8_t* base, uint32_t dev, uint32_t color_ptr, float z, uint32_t stencil) {
  Record r;
  r.op = Record::Op::kClear;
  CurrentTargets(base, dev, r.color, r.depth);
  const Image* target = r.color ? r.color : r.depth;
  if (!target) return;
  uint32_t width = target->width, height = target->height;
  if (r.color && r.depth) {
    width = std::min(r.color->width, r.depth->width);
    height = std::min(r.color->height, r.depth->height);
  }
  if (r.color && color_ptr) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    for (int i = 0; i < 4; ++i) {
      const uint32_t w = g::U32(base, color_ptr + 4 * i);
      std::memcpy(&a.clearValue.color.float32[i], &w, 4);
    }
  }
  if (r.depth) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    a.clearValue.depthStencil = {z, stencil & 0xFF};
  }
  if (!r.clears) return;
  r.rect = {{{0, 0}, {width, height}}, 0, 1};
  frame_entries.push_back({false, r, {}});
}

// Resolve: copies a render target into a texture's memory; natively, into the image that address stands for
// the flags also ask for the targets to be cleared after (0x100 colour, 0x200 depth and stencil)
void ClearAfterResolve(const uint8_t* base, uint32_t dev, uint32_t flags, uint32_t color_ptr, float z) {
  // between BeginTiling and EndTiling the clear readies EDRAM for the next tile; here the target is whole and
  // the next tile's half is still to be resolved
  if (!(flags & 0x300) || (g::Byte(base, dev + 11068) & 0x20)) return;
  Record r;
  r.op = Record::Op::kClear;
  CurrentTargets(base, dev, r.color, r.depth);
  const Image* target = r.color ? r.color : r.depth;
  if (!target) return;
  uint32_t width = target->width, height = target->height;
  if (r.color && r.depth) {
    width = std::min(r.color->width, r.depth->width);
    height = std::min(r.color->height, r.depth->height);
  }
  if ((flags & 0x100) && r.color) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    for (int i = 0; i < 4 && color_ptr; ++i) {
      const uint32_t w = g::U32(base, color_ptr + 4 * i);
      std::memcpy(&a.clearValue.color.float32[i], &w, 4);
    }
  }
  if ((flags & 0x200) && r.depth) {
    VkClearAttachment& a = r.attachments[r.clears++];
    a.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    a.clearValue.depthStencil = {z, 0};
  }
  if (!r.clears) return;
  r.rect = {{{0, 0}, {width, height}}, 0, 1};
  frame_entries.push_back({false, r, {}});
}

}  // namespace

bool NullGpu() {
  static const bool on = rex::cvar::GetFlagByName("gpu_plugin") == "native";
  return on;
}

namespace {

// a resolve the CPU reads back (the team gear, composed at load and then compressed by the game): the frame so
// far runs now, the image comes back, and goes into guest memory as the 360 would have left it there, at 1x,
// in the destination header's channel order, byte order and tiling, before the game's next call
void WriteBack(const uint32_t words[6], uint32_t address, uint32_t tex_w, uint32_t tex_h, uint32_t scale,
               Image& image) {
  if ((words[1] & 0x3F) != 6) {
    static bool told = false;
    if (!told) REXLOG_WARN("[native] a resolve read back by the CPU into format {} isn't handled", words[1] & 0x3F);
    told = true;
    return;
  }
  Assemble();
  std::vector<uint8_t> px;
  if (!FlushAndRead(image, px)) return;
  const bool tiled = words[0] >> 31;
  const uint32_t pitch = ((words[0] >> 22) & 0x1FF) * 32, endian = (words[1] >> 6) & 3;
  const uint32_t swizzle = (words[3] >> 1) & 0xFFF;
  uint8_t* dst = Physical(address);
  for (uint32_t y = 0; y < tex_h; ++y)
    for (uint32_t x = 0; x < tex_w; ++x) {
      uint32_t sum[4] = {};
      for (uint32_t sy = 0; sy < scale; ++sy)
        for (uint32_t sx = 0; sx < scale; ++sx) {
          const uint8_t* p = px.data() + (size_t(y * scale + sy) * image.width + x * scale + sx) * 4;
          for (int c = 0; c < 4; ++c) sum[c] += p[c];
        }
      uint32_t comp[4] = {};
      for (int c = 0; c < 4; ++c)
        if (const uint32_t d = swizzle >> (3 * c) & 7; d < 4) comp[d] = (sum[c] + scale * scale / 2) / (scale * scale);
      uint32_t v = comp[0] | comp[1] << 8 | comp[2] << 16 | comp[3] << 24;
      if (endian == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
      else if (endian == 2) v = __builtin_bswap32(v);
      else if (endian == 3) v = (v << 16) | (v >> 16);
      const uint32_t at = tiled ? uint32_t(rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, 2))
                                : (y * pitch + x) * 4;
      std::memcpy(dst + at, &v, 4);
    }
}

void Resolve(const uint8_t* base, uint32_t dev, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
             uint32_t dest_point, bool to_memory) {
  if (!dest_texture) return;
  Image *color, *depth;
  CurrentTargets(base, dev, color, depth);
  // the low bits pick the target: colour 0-3, or 4 for depth
  Image* src = (flags & 7) == 4 ? depth : color;
  if (!src) return;
  uint32_t words[6];
  for (int w = 0; w < 6; ++w) words[w] = g::U32(base, dest_texture + 28 + 4 * w);
  const uint32_t address = g::PhysicalAddress(words[1] & 0xFFFFF000u);
  const uint32_t tex_w = (words[2] & 0x1FFF) + 1, tex_h = (words[2] >> 13 & 0x1FFF) + 1;
  const uint32_t s = RenderScale();
  Image* dst = ResolveTexture(address, tex_w * s, tex_h * s, src->format, words);
  if (stats.tracing())
    REXLOG_INFO("[trace] resolve flags {:X} from {} {}x{} rect {} {} {} {} to {:08X} {}x{} fmt {} point {} {}", flags,
                static_cast<const void*>(src), src->width, src->height, src_rect ? int(g::U32(base, src_rect)) : -1,
                src_rect ? int(g::U32(base, src_rect + 4)) : -1, src_rect ? int(g::U32(base, src_rect + 8)) : -1,
                src_rect ? int(g::U32(base, src_rect + 12)) : -1, address, tex_w, tex_h, words[1] & 0x3F,
                dest_point ? int(g::U32(base, dest_point)) : -1, dest_point ? int(g::U32(base, dest_point + 4)) : -1);
  if (!dst) return;
  int32_t x0 = 0, y0 = 0, x1 = int32_t(tex_w), y1 = int32_t(tex_h);
  if (src_rect) {
    x0 = int32_t(g::U32(base, src_rect));
    y0 = int32_t(g::U32(base, src_rect + 4));
    x1 = int32_t(g::U32(base, src_rect + 8));
    y1 = int32_t(g::U32(base, src_rect + 12));
  }
  int32_t dx = 0, dy = 0;
  if (dest_point) {
    dx = int32_t(g::U32(base, dest_point));
    dy = int32_t(g::U32(base, dest_point + 4));
  }
  const auto clamp = [](int32_t v, uint32_t hi) { return std::clamp(v, 0, int32_t(hi)); };
  const uint32_t w = uint32_t(std::max(0, std::min({(x1 - x0) * int32_t(s), int32_t(src->width) - x0 * int32_t(s),
                                                    int32_t(dst->width) - dx * int32_t(s)})));
  const uint32_t h = uint32_t(std::max(0, std::min({(y1 - y0) * int32_t(s), int32_t(src->height) - y0 * int32_t(s),
                                                    int32_t(dst->height) - dy * int32_t(s)})));
  if (!w || !h) return;
  Record r;
  r.op = Record::Op::kResolve;
  r.color = src;
  r.resolve_to = dst;
  r.copy.srcSubresource = r.copy.dstSubresource = {Aspect(src->format), 0, 0, 1};
  r.copy.srcOffset = {clamp(x0 * int32_t(s), src->width), clamp(y0 * int32_t(s), src->height), 0};
  r.copy.dstOffset = {clamp(dx * int32_t(s), dst->width), clamp(dy * int32_t(s), dst->height), 0};
  r.copy.extent = {w, h, 1};
  frame_entries.push_back({false, r, {}});
  if (to_memory) WriteBack(words, address, tex_w, tex_h, s, *dst);
}


}  // namespace

Image* SwapImage(uint32_t front_texture_physical) {
  FlushAllInline();
  Assemble();
  TexturesFrameDone();
  ++stats.frame;
  {
    // segments linked and never run: the game dropped them
    std::lock_guard lock(segments.mutex);
    for (auto it = segments.linked.begin(); it != segments.linked.end();)
      it = it->second.frame + 2 < stats.frame ? segments.linked.erase(it) : std::next(it);
  }
  TextureTrace(stats.tracing());
  return ResolvedImage(front_texture_physical);
}

void DrawStats(uint64_t& draws, uint64_t& skipped) {
  draws = stats.draws;
  skipped = stats.skipped + stats.missed_segments;
  // per call: since the last one
  uint64_t overwritten;
  {
    std::lock_guard lock(segments.mutex);
    overwritten = segments.overwritten;
    segments.overwritten = 0;
  }
  REXLOG_INFO("[native] assembling took {:.1f} ms, of which waiting for segments {:.1f} ms; {} segments linked over "
              "one not yet drawn", stats.assemble_ms, stats.wait_ms, overwritten);
  stats.assemble_ms = stats.wait_ms = 0;
}

void FlushDevice(uint32_t dev) { FlushInline(dev); }

}  // namespace bardown::native

using namespace bardown::native;

extern "C" REX_FUNC(sub_827E5BA0) {
  const uint32_t dev = ctx.r3.u32;
  if (stats.tracing() && segment_log.fetch_add(1) <= 200)
    REXLOG_INFO("[trace] indexed draw dev {:08X} type {} n {}", dev, DeviceType(dev), ctx.r7.u32);
  ProbeDraw(base, dev, "indexed", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  if (const Route route = RouteFor(dev); route != Route::kNone) {
    FlushInline(dev);
    Draw(base, dev, route, {Kind::kIndexed, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32});
    if (NativeOnly() && !AlsoEmulated(base, dev, true)) return;
  }
  __imp__sub_827E5BA0(ctx, base);
}

extern "C" REX_FUNC(sub_827E5788) {
  const uint32_t dev = ctx.r3.u32;
  ProbeDraw(base, dev, "auto", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 0);
  if (const Route route = RouteFor(dev); route != Route::kNone) {
    FlushInline(dev);
    Draw(base, dev, route, {Kind::kAuto, ctx.r4.u32, 0, ctx.r5.u32, ctx.r6.u32});
    if (NativeOnly() && !AlsoEmulated(base, dev, true)) return;
  }
  __imp__sub_827E5788(ctx, base);
}

// BeginVertices: natively the game writes its vertices into space of the renderer's own, so nothing goes into
// the D3D ring and no draw reaches the emulated GPU. handed out in turn from one block; a draw's vertices are
// converted at the device's next call, long before the block comes round again
uint32_t InlineSpace(uint32_t bytes) {
  constexpr uint32_t kSize = 4u << 20;
  static std::mutex mutex;
  static uint32_t block = 0, used = 0;
  std::lock_guard lock(mutex);
  if (!block && !(block = GuestAlloc(kSize))) return 0;
  bytes = (bytes + 255) & ~255u;
  if (bytes > kSize) return 0;
  if (used + bytes > kSize) used = 0;
  const uint32_t at = block + used;
  used += bytes;
  return at;
}

extern "C" REX_FUNC(sub_827E52B8) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  ProbeDraw(base, dev, "inline", prim, count, stride, 0);
  const Route route = RouteFor(dev);
  if (route != Route::kNone) FlushInline(dev);
  if (route != Route::kNone && NativeOnly()) {
    if (const uint32_t at = InlineSpace(count * stride)) {
      // what the original leaves in the device besides returning it: the game writes through these too
      // +13844 is where the ring cursor (+48) goes when the game ends the vertices (callers do it inline): the
      // ring wasn't used, so it stays where it is
      g::SetU32(base, dev + 13844, g::U32(base, dev + 48));
      g::SetU32(base, dev + 13848, at);
      g::SetU32(base, dev + 13856, count * stride / 4);
      ctx.r3.u32 = at;
      Draw(base, dev, route, {Kind::kInline, prim, 0, 0, count, stride, at});
      return;
    }
  }
  __imp__sub_827E52B8(ctx, base);
  // r3 is now the space the game is about to fill
  if (route != Route::kNone && ctx.r3.u32)
    Draw(base, dev, route, {Kind::kInline, prim, 0, 0, count, stride, ctx.r3.u32});
}

extern "C" REX_FUNC(sub_827E5778) {
  // the matching BeginVertices didn't take ring space natively, so there's no end to move to
  if (NativeOnly() && RouteFor(ctx.r3.u32) != Route::kNone) return;
  __imp__sub_827E5778(ctx, base);
}

extern "C" REX_FUNC(sub_827DFB60) {
  const uint32_t dev = ctx.r3.u32;
  if (const Route route = RouteFor(dev); route != Route::kNone) {
    FlushInline(dev);
    Clear(base, dev, route, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r8.u32);
    if (NativeOnly() && !AlsoEmulated(base, dev, false)) return;
  }
  __imp__sub_827DFB60(ctx, base);
}

extern "C" REX_FUNC(sub_827E6760) {
  const uint32_t dev = ctx.r3.u32;
  // resolves come from the main device
  if (RouteFor(dev) == Route::kNow) {
    FlushInline(dev);
    if (stats.tracing())
      REXLOG_INFO("[trace] resolve from {:08X} tiles {} flag {:X}", uint32_t(ctx.lr), g::U32(base, dev + 13124),
                  g::Byte(base, dev + 11068));
    const uint32_t flags = ctx.r4.u32, rect = ctx.r5.u32, texture = ctx.r6.u32, point = ctx.r7.u32,
                   clear_color = ctx.r10.u32;
    const float z = float(ctx.f1.f64);
    // native only: the emulated GPU has drawn nothing to copy out, unless it drew a composition. with the
    // emulated GPU gone (the native plugin) the composition is copied out natively instead
    const bool compose = NativeOnly() && AlsoEmulated(base, dev, false);
    if (!NativeOnly() || (compose && !NullGpu())) __imp__sub_827E6760(ctx, base);
    if (compose) ResolvedOut(dev);
    Resolve(base, dev, flags, rect, texture, point, compose && NullGpu());
    ClearAfterResolve(base, dev, flags, clear_color, z);
    return;
  } else if (Drawing() && DeviceType(dev) == 2) {
    static std::atomic<bool> told{false};
    if (!told.exchange(true)) REXLOG_WARN("[native] a worker device resolved; that isn't drawn yet");
  }
  __imp__sub_827E6760(ctx, base);
}

extern "C" REX_FUNC(sub_827E2148) {
  const uint32_t resource = ctx.r3.u32;
  if (Drawing() && resource && (g::U32(base, resource) & 0xF) == 3)
    TextureWritten(g::PhysicalAddress(g::U32(base, resource + 32) & 0xFFFFF000u));
  __imp__sub_827E2148(ctx, base);
}

namespace {

void SegmentTrace(const char* what, uint32_t dev, uint32_t segment) {
  if (!stats.tracing() || segment_log.fetch_add(1) > 200) return;
  REXLOG_INFO("[trace] segment {} dev {:08X} type {} seg {:08X}", what, dev, DeviceType(dev), segment);
}

}  // namespace

extern "C" REX_FUNC(sub_827E0038) {
  const uint32_t dev = ctx.r3.u32, segment = ctx.r4.u32;
  SegmentTrace("begin", dev, segment);
  if (Drawing()) {
    std::lock_guard lock(segments.mutex);
    segments.recording[dev].clear();
  }
  __imp__sub_827E0038(ctx, base);
}

extern "C" REX_FUNC(sub_827E05A8) {
  const uint32_t dev = ctx.r3.u32;
  SegmentTrace("end", dev, g::U32(base, dev + 13920));
  FlushInline(dev);
  if (Drawing()) {
    std::lock_guard lock(segments.mutex);
    auto it = segments.recording.find(dev);
    if (it != segments.recording.end()) {
      finished_segment = std::move(it->second);
      has_finished_segment = true;
      segments.recording.erase(it);
    }
  }
  __imp__sub_827E05A8(ctx, base);
}

// the main device leaves a slot for a segment; r4 is the link object the worker will fill it through
extern "C" REX_FUNC(sub_827E1200) {
  const uint32_t main = ctx.r3.u32, link = ctx.r4.u32;
  SegmentTrace("run", main, link);
  if (RouteFor(main) == Route::kNow) {
    Entry e;
    e.is_slot = true;
    e.slot.link = link;
    // what the segment inherits (+24 VS, +32 PS constants, a bit per 4 registers), and of that, what the main
    // device has changed and flushes before it
    e.slot.vs_inherit = g::U64(base, link + 24);
    e.slot.ps_inherit = g::U64(base, link + 32);
    e.slot.vs_dirty = g::U64(base, main) & e.slot.vs_inherit;
    e.slot.ps_dirty = g::U64(base, main + 8) & e.slot.ps_inherit;
    e.slot.main_consts.resize(8192);
    SwapWords(e.slot.main_consts.data(), g::Host(base, main + g::kVsConsts), 1024);
    SwapWords(e.slot.main_consts.data() + 4096, g::Host(base, main + g::kPsConsts), 1024);
    frame_entries.push_back(std::move(e));
  }
  __imp__sub_827E1200(ctx, base);
}

// on the worker thread, after the segment ends: r3 is the link object the main device left a slot for
extern "C" REX_FUNC(sub_827E18B0) {
  const uint32_t link = ctx.r3.u32;
  if (Drawing() && has_finished_segment) {
    {
      std::lock_guard lock(segments.mutex);
      auto [it, fresh] = segments.linked.try_emplace(link);
      if (!fresh) ++segments.overwritten;
      it->second = {std::move(finished_segment), stats.frame};
    }
    finished_segment.clear();
    has_finished_segment = false;
    segments.linked_cv.notify_all();
  }
  __imp__sub_827E18B0(ctx, base);
}

// BeginTiling clears each tile as it starts it (colour if it's given one, depth and stencil always); the
// native targets are whole, so they're cleared once. the float takes r8's place, so the stencil is in r9
extern "C" REX_FUNC(sub_827EB308) {
  const uint32_t dev = ctx.r3.u32, color = ctx.r7.u32, stencil = ctx.r9.u32;
  const float z = float(ctx.f1.f64);
  __imp__sub_827EB308(ctx, base);
  if (RouteFor(dev) == Route::kNow) ClearWhole(base, dev, color, z, stencil);
}


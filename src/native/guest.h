#pragma once

// reading the game's Direct3D device (docs/D3D.md). everything in guest memory is big-endian

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace bardown::native::guest {

// a guest address in host memory. on Windows (and Apple Silicon) the runtime maps 0xE0000000 and up 4 KB further
// along, and the game's own loads and stores add that (REX_PHYS_HOST_OFFSET); reads from here have to as well
#if defined(_WIN32) || (defined(__APPLE__) && defined(__aarch64__))
inline constexpr uint32_t HostOffset(uint32_t addr) { return addr >= 0xE0000000u ? 0x1000u : 0u; }
#else
inline constexpr uint32_t HostOffset(uint32_t) { return 0; }
#endif
inline uint8_t* Host(uint8_t* base, uint32_t addr) { return base + addr + HostOffset(addr); }
inline const uint8_t* Host(const uint8_t* base, uint32_t addr) { return base + addr + HostOffset(addr); }
inline uint8_t Byte(const uint8_t* base, uint32_t addr) { return *Host(base, addr); }

inline uint32_t U32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, Host(base, addr), 4);
  return __builtin_bswap32(v);
}
inline void SetU32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Host(base, addr), &value, 4);
}
inline uint64_t U64(const uint8_t* base, uint32_t addr) {
  uint64_t v;
  std::memcpy(&v, Host(base, addr), 8);
  return __builtin_bswap64(v);
}
inline uint16_t U16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, Host(base, addr), 2);
  return __builtin_bswap16(v);
}

// device layout
inline constexpr uint32_t kFetch = 0x480;        // 32 fetch constants x 6 dwords
inline constexpr uint32_t kVsConsts = 0x780;     // c0-c255
inline constexpr uint32_t kPsConsts = 0x1780;    // c0-c255
inline constexpr uint32_t kBoolConsts = 0x2780;  // bools then loops
inline constexpr uint32_t kDecl = 0x2FD0;
inline constexpr uint32_t kIndices = 0x320C;
inline constexpr uint32_t kTargets = 0x3210;  // 4 colour surfaces
inline constexpr uint32_t kDepth = 0x3220;
inline constexpr uint32_t kStreams = 0x3224;  // 16 vertex buffers
inline constexpr uint32_t kTextures = 0x3278;
inline constexpr uint32_t kPixelShader = 0x330C;
inline constexpr uint32_t kVertexShader = 0x3310;
inline constexpr uint32_t kDeclStride = 0x2FE8;    // per stream, a byte: the declaration's stride / 4
inline constexpr uint32_t kStreamStride = 0x3268;  // per stream, a byte: SetStreamSource's stride / 4, 0 = the declaration's

// the device keeps a shadow of the GPU registers in eight blocks
inline uint32_t RegOffset(uint32_t reg) {
  struct Block {
    uint32_t first, offset, count;
  };
  static constexpr Block blocks[] = {{0x2000, 0x2880, 16}, {0x2080, 0x28C0, 3},  {0x2100, 0x28CC, 21},
                                     {0x2180, 0x2920, 5},  {0x2200, 0x2934, 12}, {0x2280, 0x2964, 21},
                                     {0x2300, 0x29B8, 38}, {0x2380, 0x2A50, 8}};
  const auto* b = std::find_if(std::begin(blocks), std::end(blocks),
                               [&](const Block& x) { return reg >= x.first && reg < x.first + x.count; });
  return b == std::end(blocks) ? 0 : b->offset + (reg - b->first) * 4;
}
inline uint32_t Reg(const uint8_t* base, uint32_t device, uint32_t reg) {
  const uint32_t off = RegOffset(reg);
  return off ? U32(base, device + off) : 0;
}

// texture headers keep the virtual address; D3D turns it physical when it binds one (the top 512 MB view is
// offset by a page)
inline uint32_t PhysicalAddress(uint32_t virt) {
  return ((((virt >> 20) + 0x200) & 0x1000) + (virt & 0x1FFFFFFF)) & 0x1FFFFFFF;
}

// vertex fetch for a stream: two dwords at the top of the fetch area, going down
inline uint32_t StreamFetch(uint32_t stream) { return 0x778 - 8 * stream; }

}  // namespace bardown::native::guest

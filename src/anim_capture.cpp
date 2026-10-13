// records the animation decoder's inputs and outputs:
//   BARDOWN_ANIM_CAPTURE=<dir> [BARDOWN_ANIM_CAPTURE_MAX=3000] scripts/run.sh match
//
// sub_82C9BA88(state r3, r4, frame r5, out r6) decodes one clip at one frame. per call <dir>/anim.bin gets:
//   "ANIM", r3 r4 r5 r6 (u32 LE), 64 bytes at r3, then for each word there that points at readable
//   memory: u32 address + 512 bytes from it, closed by a 0 address; then 2048 bytes at r6 after the call.
// guest bytes are stored as they are (big-endian).

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

#include "load_watch.h"

REX_EXTERN(__imp__sub_82C9BA88);

namespace {

struct Capture {
  std::mutex mutex;
  std::ofstream out;
  long left = 0;
  uintptr_t membase = 0;
  std::vector<std::pair<uint32_t, uint32_t>> readable;
  bool ready = false;
};

Capture& State(uint8_t* base) {
  static Capture c;
  if (!c.ready) {
    c.ready = true;
    const char* dir = std::getenv("BARDOWN_ANIM_CAPTURE");
    if (dir && *dir) {
      c.out.open(std::string(dir) + "/anim.bin", std::ios::binary);
      const char* max = std::getenv("BARDOWN_ANIM_CAPTURE_MAX");
      c.left = max ? std::atol(max) : 3000;
      c.membase = uintptr_t(base);
    }
  }
  return c;
}

bool Readable(Capture& c, uint32_t addr, uint32_t len) {
  for (int pass = 0; pass < 2; ++pass) {
    for (auto [lo, size] : c.readable)
      if (addr >= lo && uint64_t(addr) + len <= uint64_t(lo) + size) return true;
    if (pass == 0) c.readable = bardown::ReadableGuestRanges(c.membase);
  }
  return false;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }


}  // namespace

extern "C" REX_FUNC(sub_82C9BA88) {
  auto& c = State(base);
  if (c.left <= 0) {
    __imp__sub_82C9BA88(ctx, base);
    return;
  }
  uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  std::vector<char> record;
  {
    std::lock_guard lock(c.mutex);
    if (Readable(c, r3, 64)) {
      auto add = [&](const void* p, size_t n) {
        record.insert(record.end(), static_cast<const char*>(p), static_cast<const char*>(p) + n);
      };
      add("ANIM", 4);
      for (uint32_t v : {r3, r4, r5, r6}) add(&v, 4);
      const uint8_t* s = base + r3;
      add(s, 64);
      for (int i = 0; i < 16; ++i) {
        uint32_t ptr = Be32(s + i * 4);
        if (ptr < 0x10000 || !Readable(c, ptr, 512)) continue;
        add(&ptr, 4);
        add(base + ptr, 512);
      }
      uint32_t zero = 0;
      add(&zero, 4);
    }
  }
  __imp__sub_82C9BA88(ctx, base);
  if (record.empty()) return;
  std::lock_guard lock(c.mutex);
  if (c.left <= 0 || !Readable(c, r6, 2048)) return;
  --c.left;
  c.out.write(record.data(), std::streamsize(record.size()));
  c.out.write(reinterpret_cast<const char*>(base + r6), 2048);
  if (c.left == 0) c.out.flush();
}

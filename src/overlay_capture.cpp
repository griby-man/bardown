// the data strings the HUD overlays get, logged:
//   BARDOWN_OVERLAY_LOG=<dir> scripts/run.sh match
// <dir>/overlay.txt, one line per formatted string (sub_826DAD98 = format into a fixed string buffer,
// which the overlay caches use to build the "a|b|c" data their movies split):
//   <ms> <tick> <caller lr> <format> => <result>      ms = steady clock, same as BARDOWN_SHOT_STOPPAGE names
// repeats of the same (caller, result) are dropped

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_826DAD98);

namespace {

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

std::string Str(const uint8_t* base, uint32_t a, uint32_t end = 0) {
  std::string s;
  if (!Ok(a)) return "-";
  for (uint32_t i = 0; i < 400; ++i) {
    if (end && a + i >= end) break;
    char c = char(base[a + i]);
    if (!c) break;
    s += (c == '\n' ? '/' : c);
  }
  return s;
}

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_OVERLAY_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/overlay.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

std::mutex g_lock;
std::map<uint32_t, std::string> g_last;  // per caller

}  // namespace

// r3 string object (+0 buffer, +4 write position), r4 format, varargs after
extern "C" REX_FUNC(sub_826DAD98) {
  const uint32_t obj = ctx.r3.u32, fmt = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_826DAD98(ctx, base);
  FILE* o = Out();
  if (!o) return;
  const std::string f = Str(base, fmt);
  const uint32_t b = L32(base, obj), e = L32(base, obj + 4);
  const std::string r = Str(base, b, e > b && e - b < 4096 ? e : 0);
  std::lock_guard<std::mutex> g(g_lock);
  auto& last = g_last[lr];
  if (last == r) return;
  last = r;
  const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch()).count());
  std::fprintf(o, "%ld %u %08x %s => %s\n", ms, L32(base, 0x83C75BF8 + 168), lr, f.c_str(), r.c_str());
  static uint32_t n = 0;
  if (++n % 20 == 0) std::fflush(o);
}

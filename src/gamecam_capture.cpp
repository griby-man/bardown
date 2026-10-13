// gameplay ICE camera (Camera_GameplayICE update sub_83448220), logged:
//   BARDOWN_GAMECAM_LOG=<dir> -> <dir>/gamecam.txt
//   C <ms> <tick> <sitcom> <fomgr> | dir <fields> | cam <state after the update> | own <idx> <pos> <vel>
//   K <clamped target from sub_83441010>  (written just before the C line of the same update)
//   S <camsys 96 444 448 416>  every 300th update

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_83448220);
REX_EXTERN(__imp__sub_83441010);

namespace {

const auto g_start = std::chrono::steady_clock::now();

uint32_t L32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
float LF(const uint8_t* b, uint32_t a) {
  uint32_t v = L32(b, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_GAMECAM_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/gamecam.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

void V3(FILE* o, const uint8_t* b, uint32_t a) {
  std::fprintf(o, " %.4f %.4f %.4f", LF(b, a), LF(b, a + 4), LF(b, a + 8));
}

uint32_t g_n = 0;
bool g_in = false;

}  // namespace

extern "C" REX_FUNC(sub_83441010) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_83441010(ctx, base);
  if (FILE* o = Out(); o && g_in && Ok(out)) {
    std::fprintf(o, "K");
    V3(o, base, out);
    std::fprintf(o, "\n");
  }
}

extern "C" REX_FUNC(sub_83448220) {
  const uint32_t cam = ctx.r3.u32;
  g_in = true;
  __imp__sub_83448220(ctx, base);
  g_in = false;
  FILE* o = Out();
  if (!o || !Ok(cam)) return;
  const uint32_t d = L32(base, 0x83C7F038);
  if (!Ok(d)) return;
  const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
  std::fprintf(o, "C %ld %u %d %d | dir dt %.4f o788 %d f833 %u f836 %u f840 %u o848 %d r868 %d f872 %d s884 %d f1074 %u f1166 %u o1168 %d f1173 %u f800 %u o1392 %d",
               ms, L32(base, 0x83C75BF8 + 168), int32_t(L32(base, 0x83BFD050)), int32_t(L32(base, 0x83C75C40)),
               LF(base, d + 4), int32_t(L32(base, d + 788)), base[d + 833], base[d + 836], base[d + 840],
               int32_t(L32(base, d + 848)), int32_t(L32(base, d + 868)), int32_t(L32(base, d + 872)),
               int32_t(L32(base, d + 884)), base[d + 1074], base[d + 1166], int32_t(L32(base, d + 1168)), base[d + 1173],
               base[d + 800], int32_t(L32(base, d + 1392)));
  std::fprintf(o, " p112");
  V3(o, base, d + 112);
  std::fprintf(o, " p128");
  V3(o, base, d + 128);
  std::fprintf(o, " p1088");
  V3(o, base, d + 1088);
  std::fprintf(o, " b1124 %.4f", LF(base, d + 1124));
  // cam: eye16 ang32 fov48 timer64 | s80: pos96 vel112 lag128 | fov144: cur148 | lead160: tgt176 base192 zoff208 st212 held216 tm220 | out224 r248 r252 | s256: pos272 vel288
  std::fprintf(o, " | eye");
  V3(o, base, cam + 16);
  std::fprintf(o, " ang");
  V3(o, base, cam + 32);
  std::fprintf(o, " fov %.5f tm %.3f a1", LF(base, cam + 48), LF(base, cam + 64));
  V3(o, base, cam + 96);
  std::fprintf(o, " v1");
  V3(o, base, cam + 112);
  std::fprintf(o, " lag %.4f fc %.5f tgt", LF(base, cam + 128), LF(base, cam + 148));
  V3(o, base, cam + 176);
  std::fprintf(o, " bas");
  V3(o, base, cam + 192);
  std::fprintf(o, " zo %.4f st %d %d %.3f r %.5f %.5f a2", LF(base, cam + 208), int32_t(L32(base, cam + 212)),
               int32_t(L32(base, cam + 216)), LF(base, cam + 220), LF(base, cam + 248), LF(base, cam + 252));
  V3(o, base, cam + 272);
  std::fprintf(o, " v2");
  V3(o, base, cam + 288);
  const int32_t own = int32_t(L32(base, d + 788));
  std::fprintf(o, " | own %d", own);
  if (own >= 0 && own < 40) {
    const uint32_t p = L32(base, 0x83BFD1F0 + 4 * own);
    if (Ok(p)) {
      const uint32_t body = L32(base, p + 32);
      if (Ok(body)) {
        V3(o, base, body + 48);
        V3(o, base, body + 80);
      }
    }
  }
  const uint32_t pm = L32(base, 0x83BFD14C);
  if (Ok(pm)) {
    std::fprintf(o, " | pk");
    V3(o, base, pm + 64);
    std::fprintf(o, " %d", int32_t(L32(base, pm + 180)));
  }
  std::fprintf(o, "\n");
  if (++g_n % 300 == 1) {
    const uint32_t cs = L32(base, 0x83B4DF00);
    if (Ok(cs))
      std::fprintf(o, "S %d %d %d %d\n", int32_t(L32(base, cs + 96)), int32_t(L32(base, cs + 444)),
                   int32_t(L32(base, cs + 448)), int32_t(L32(base, cs + 416)));
    std::fflush(o);
  }
}

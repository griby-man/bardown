// skater move probes: BARDOWN_MOVES_LOG=<dir> -> <dir>/moves.txt
// every line: <tag> <tick> ...; H = human (pad-driven, +556 < 0x80) player
//   S  tick act(+60) stk(+144) energy(+360) boost(B+0) straight(B+12) next(B+16) btn(B+32) | on change, or every 30 ticks
//   AQ request action  (sub_83545800 r4 r5 r6)        AC clear request (sub_8351FA28 r4 r5 r6)
//   AN anim command    (sub_83329B20 cmd flag)        (human only)
//   SL/PK/CH/SW/HC/DV  stick lift / plain poke / aimed chop / sweep / hip-check start / dive end, with result
//   SR saucer request (f1 power, r6 type, r7 rcv) -> block +752 +812 +768.. +820, D+168
//   PR pass request (f1, r5 kind) -> block +752 +812
//   RL pass release -> block +752 +812 and R pos/vel
//   BU boost update (human): r4 f1 f2 f3 f4 r9 -> B+0 +4 +12 +16 +32 +33
//   LC line change cycle (team, hud)   PG pull goalie (team, ctrl) -> r3   IM input message (sub_835F6058 r4)
//   PP protect decision (r4 r5 r6) -> r3, interp out

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_83545800);
REX_EXTERN(__imp__sub_8351FA28);
REX_EXTERN(__imp__sub_83329B20);
REX_EXTERN(__imp__sub_835444A8);
REX_EXTERN(__imp__sub_83543988);
REX_EXTERN(__imp__sub_83529F98);
REX_EXTERN(__imp__sub_83529B98);
REX_EXTERN(__imp__sub_8350F1C8);
REX_EXTERN(__imp__sub_8351E840);
REX_EXTERN(__imp__sub_83434A00);
REX_EXTERN(__imp__sub_834360A0);
REX_EXTERN(__imp__sub_83423CE0);
REX_EXTERN(__imp__sub_828FB9F0);
REX_EXTERN(__imp__sub_83500148);
REX_EXTERN(__imp__sub_833E5798);
REX_EXTERN(__imp__sub_83508258);
REX_EXTERN(__imp__sub_835F6058);
REX_EXTERN(__imp__sub_83332BC0);

namespace {

uint32_t L32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
float LF(const uint8_t* b, uint32_t a) {
  uint32_t w = L32(b, a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }
uint32_t Tick(const uint8_t* b) { return L32(b, 0x83C75BF8 + 168); }

std::mutex g_lock;
FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_MOVES_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/moves.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

uint32_t Human(const uint8_t* b) {
  for (int i = 0; i < 64; ++i) {
    const uint32_t p = L32(b, 0x83BFD1F0 + 4 * i);
    if (Ok(p) && b[p + 556] < 0x80 && L32(b, p + 40) == 0) return p;
  }
  return 0;
}
uint32_t Boost(const uint8_t* b, uint32_t p) {
  const uint32_t a = L32(b, p + 80);
  return Ok(a) ? L32(b, a + 96) : 0;
}

#define LOG(...)                                \
  do {                                          \
    if (FILE* o = Out()) {                      \
      std::lock_guard hold(g_lock);             \
      std::fprintf(o, __VA_ARGS__);             \
      std::fflush(o);                           \
    }                                           \
  } while (0)

}  // namespace

extern "C" REX_FUNC(sub_83545800) {
  const uint32_t h = Human(base), obj = ctx.r3.u32;
  const bool hum = h && Ok(L32(base, h + 508)) && L32(base, L32(base, h + 508) + 136) == obj;
  if (hum) LOG("AQ %u %u %u %u\n", Tick(base), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_83545800(ctx, base);
}
extern "C" REX_FUNC(sub_8351FA28) {
  const uint32_t h = Human(base), obj = ctx.r3.u32;
  const bool hum = h && Ok(L32(base, h + 508)) && L32(base, L32(base, h + 508) + 136) == obj;
  if (hum) LOG("AC %u %u %u %u\n", Tick(base), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_8351FA28(ctx, base);
}
extern "C" REX_FUNC(sub_83329B20) {
  const uint32_t h = Human(base);
  if (h && L32(base, h + 80) == ctx.r3.u32) LOG("AN %u %u %u\n", Tick(base), ctx.r4.u32, ctx.r5.u32);
  __imp__sub_83329B20(ctx, base);
}

#define PLAYER_HOOK(fn, tag)                                                                        \
  extern "C" REX_FUNC(fn) {                                                                         \
    const uint32_t p = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;                                \
    const float a = Ok(r4) ? LF(base, r4) : 0.f;                                                    \
    __imp__##fn(ctx, base);                                                                         \
    if (p == Human(base))                                                                           \
      LOG(tag " %u r4 %08x(%g) r5 %u -> %u act %u stk %u\n", Tick(base), r4, a, r5, ctx.r3.u32 & 0xFF, \
          L32(base, p + 60), L32(base, p + 144));                                                   \
  }
PLAYER_HOOK(sub_835444A8, "SL")
PLAYER_HOOK(sub_83543988, "PK")
PLAYER_HOOK(sub_83529F98, "CH")
PLAYER_HOOK(sub_83529B98, "SW")
PLAYER_HOOK(sub_8351E840, "DV")

// sub_8350F1C8(ctrl, player, blk, g, r7 manual, &cancel)
extern "C" REX_FUNC(sub_8350F1C8) {
  const uint32_t p = ctx.r4.u32, r7 = ctx.r7.u32;
  __imp__sub_8350F1C8(ctx, base);
  if (p == Human(base))
    LOG("HC %u manual %u -> %u act %u stk %u\n", Tick(base), r7, ctx.r3.u32 & 0xFF, L32(base, p + 60),
        L32(base, p + 144));
}

constexpr uint32_t kBlk = 0x83C745D0;
void Block(const uint8_t* b, const char* tag) {
  LOG("%s %u sp %.6f kind %u T %.4f %.4f %.4f head %.5f rcv %08x D168 %u\n", tag, Tick(b), LF(b, kBlk + 752),
      L32(b, kBlk + 812), LF(b, kBlk + 768), LF(b, kBlk + 772), LF(b, kBlk + 776), LF(b, kBlk + 820),
      L32(b, kBlk + 824), Ok(L32(b, 0x83BFD04C)) ? L32(b, L32(b, 0x83BFD04C) + 168) : 9999);
}

extern "C" REX_FUNC(sub_83434A00) {
  const uint32_t p = ctx.r3.u32;
  const double pw = ctx.f1.f64;
  const uint32_t r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  const float aim = Ok(ctx.r4.u32) ? LF(base, ctx.r4.u32) : 0.f;
  __imp__sub_83434A00(ctx, base);
  LOG("SR %u p %08x hum %d pw %.5f type %u rcv %08x aim %.5f -> %u\n", Tick(base), p, p == Human(base), pw, r6, r7,
      aim, ctx.r3.u32 & 0xFF);
  Block(base, "SB");
}
extern "C" REX_FUNC(sub_834360A0) {
  const uint32_t p = ctx.r3.u32, r5 = ctx.r5.u32;
  const double pw = ctx.f1.f64;
  __imp__sub_834360A0(ctx, base);
  LOG("PR %u p %08x hum %d pw %.5f kind %u -> %u\n", Tick(base), p, p == Human(base), pw, r5, ctx.r3.u32 & 0xFF);
  Block(base, "PB");
}
extern "C" REX_FUNC(sub_83423CE0) {
  const uint32_t p = ctx.r3.u32;
  __imp__sub_83423CE0(ctx, base);
  const uint32_t pm = L32(base, 0x83BFD14C);
  const uint32_t r = Ok(pm) ? L32(base, pm + 32) : 0;
  LOG("RL %u p %08x hum %d", Tick(base), p, p == Human(base));
  if (Ok(r))
    LOG(" Rpos %.4f %.4f %.4f Rvel %.6f %.6f %.6f\n", LF(base, r + 48), LF(base, r + 52), LF(base, r + 56),
        LF(base, r + 80), LF(base, r + 84), LF(base, r + 88));
  else
    LOG("\n");
  Block(base, "RB");
}

extern "C" REX_FUNC(sub_828FB9F0) {
  const uint32_t B = ctx.r3.u32, h = Human(base);
  const bool hum = h && Boost(base, h) == B;
  const uint32_t r4 = ctx.r4.u32, r9 = ctx.r9.u32;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64, f3 = ctx.f3.f64, f4 = ctx.f4.f64;
  const float b0 = hum ? LF(base, B) : 0.f;
  __imp__sub_828FB9F0(ctx, base);
  if (!hum) return;
  static float last = -1;
  static uint32_t lastr4 = 9;
  const float b1 = LF(base, B);
  if (b1 != last || r4 != lastr4 || b0 != b1)
    LOG("BU %u hus %u en %.4f spd %.4f acc %.4f eff %.4f now %u | B0 %.5f low %u str %u next %d btn %u clk %u\n",
        Tick(base), r4 & 0xFF, f1, f2, f3, f4, r9, b1, L32(base, B + 4), L32(base, B + 12), int32_t(L32(base, B + 16)),
        base[B + 32], base[B + 33]);
  last = b1;
  lastr4 = r4;
}

extern "C" REX_FUNC(sub_83500148) {
  const uint32_t p = ctx.r3.u32;
  const bool hum = p && p == Human(base);
  const float e0 = hum ? LF(base, p + 360) : 0.f;
  __imp__sub_83500148(ctx, base);
  if (!hum) return;
  static uint32_t la = 99, ls = 99, lt = 0;
  const uint32_t act = L32(base, p + 60), stk = L32(base, p + 144), t = Tick(base);
  const uint32_t B = Boost(base, p);
  if (act != la || stk != ls || t - lt >= 30) {
    LOG("S %u act %u stk %u en %.5f -> %.5f B0 %.5f\n", t, act, stk, e0, LF(base, p + 360), B ? LF(base, B) : 0.f);
    la = act, ls = stk, lt = t;
  }
}

extern "C" REX_FUNC(sub_833E5798) {
  LOG("LC %u team %u hud %u r6 %u r7 %u\n", Tick(base), ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_833E5798(ctx, base);
}
extern "C" REX_FUNC(sub_83508258) {
  const uint32_t t = ctx.r3.u32, c = ctx.r4.u32;
  __imp__sub_83508258(ctx, base);
  LOG("PG %u team %u ctrl %u -> %u\n", Tick(base), t, c, ctx.r3.u32 & 0xFF);
}
extern "C" REX_FUNC(sub_835F6058) {
  LOG("IM %u %u\n", Tick(base), ctx.r4.u32);
  __imp__sub_835F6058(ctx, base);
}
extern "C" REX_FUNC(sub_83332BC0) {
  const uint32_t p = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32;
  __imp__sub_83332BC0(ctx, base);
  static uint32_t last = 9;
  if (p == Human(base) && (ctx.r3.u32 & 0xFF) != last) {
    last = ctx.r3.u32 & 0xFF;
    LOG("PP %u side %u r5 %u -> %u interp %g\n", Tick(base), r4 & 0xFF, r5, last, Ok(r6) ? LF(base, r6) : -1.f);
  }
}

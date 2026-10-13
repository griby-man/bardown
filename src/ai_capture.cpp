// skater AI decisions, for mapping the original's AI:
//   BARDOWN_AI_LOG=<dir> -> <dir>/ai.txt
// T lines: once per player think (sub_835024C0, the per-player AI tick), after it ran:
//   T tick P idx team ctrl pos label rush job jobticks exe1664 exe1668 x z cmd teamflags play
//   cmd = P+1676 (command id from the team's play), teamflags = team(P+1600)+56, play = play manager
//   (*(*0x83BFE7C8 + 424) + 48) + team*1088 + 16
//   label = P+1652 strategic label, rush = P+1656, job = P+1672, jobticks = P+1692,
//   exe = vtable of the executor object the trees stored (0 none). pos = P+192 -> +41
// S lines: every skate-to request (sub_833ADC98), with the return address of whoever called
//   the wrapper (sub_833AF300 / sub_833AF238):
//   S tick P caller x z effort gap arrive ease f4 owner c1 c2 c3 c4  (c = return addresses up the guest stack)
// D lines: raw dump of each team object (P+1600) every BARDOWN_AI_DUMP ticks (default off)

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_835024C0);
REX_EXTERN(__imp__sub_833ADC98);
REX_EXTERN(__imp__sub_833AF300);
REX_EXTERN(__imp__sub_833AF238);

namespace {

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
uint32_t Tick(const uint8_t* b) { return L32(b, 0x83C75CA0); }

std::mutex g_mu;
FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_AI_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/ai.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

thread_local uint32_t t_outer = 0;

int TeamOf(const uint8_t* b, uint32_t p) {
  for (uint32_t t = 0; t < 2; ++t) {
    uint32_t team = L32(b, 0x83BFD09C + 4 * t);
    uint32_t m = Ok(team) ? L32(b, team + 88) : 0;
    for (int k = 0; Ok(m) && k < 40; ++k, m = L32(b, m + 616))
      if (m == p) return int(t);
  }
  return 2;
}

}  // namespace

extern "C" REX_FUNC(sub_835024C0) {
  const uint32_t p = ctx.r3.u32;
  __imp__sub_835024C0(ctx, base);
  FILE* o = Out();
  if (!o || !Ok(p)) return;
  uint32_t body = L32(base, p + 32), info = L32(base, p + 192);
  uint32_t e1 = L32(base, p + 1664), e2 = L32(base, p + 1668);
  uint32_t v1 = Ok(e1) ? L32(base, e1) : 0, v2 = Ok(e2) ? L32(base, e2) : 0;
  float x = Ok(body) ? LF(base, body + 48) : 0, z = Ok(body) ? LF(base, body + 56) : 0;
  std::lock_guard<std::mutex> lk(g_mu);
  uint32_t team = L32(base, p + 1600), mgr = 0, play = 0xFFFFFFFFu;
  if (uint32_t g = L32(base, 0x83BFE7C8); Ok(g))
    if (uint32_t h = L32(base, g + 424); Ok(h)) mgr = L32(base, h + 48);
  if (Ok(mgr)) play = L32(base, mgr + uint32_t(int8_t(base[p + 48])) * 1088 + 16);
  std::fprintf(o, "T %u %08x %d %d %u %u %d %d %d %d %08x %08x %.3f %.3f %d %08x %d\n", Tick(base), p, int(int8_t(base[p + 57])),
               TeamOf(base, p), unsigned(base[p + 556]), Ok(info) ? unsigned(base[info + 41]) : 255u,
               int(L32(base, p + 1652)), int(L32(base, p + 1656)), int(L32(base, p + 1672)),
               int(L32(base, p + 1692)), v1, v2, x, z, int(L32(base, p + 1676)), Ok(team) ? L32(base, team + 56) : 0u,
               int(play));
  static const long every = [] {
    const char* s = std::getenv("BARDOWN_AI_DUMP");
    return s ? std::atol(s) : 0L;
  }();
  static uint32_t last_dump = 0;
  uint32_t now = Tick(base);
  if (every > 0 && now >= last_dump + uint32_t(every)) {
    last_dump = now;
    for (uint32_t t = 0; t < 2; ++t) {
      uint32_t tobj = L32(base, 0x83BFD09C + 4 * t);
      if (!Ok(tobj)) continue;
      std::fprintf(o, "D %u team%u %08x", now, t, tobj);
      for (uint32_t k = 0; k < 2048; k += 4) std::fprintf(o, " %08x", L32(base, tobj + k));
      std::fprintf(o, "\n");
    }
  }
}

extern "C" REX_FUNC(sub_833AF300) {
  t_outer = uint32_t(ctx.lr);
  __imp__sub_833AF300(ctx, base);
  t_outer = 0;
}

extern "C" REX_FUNC(sub_833AF238) {
  t_outer = uint32_t(ctx.lr) | 1u;  // odd = came through the second wrapper
  __imp__sub_833AF238(ctx, base);
  t_outer = 0;
}

extern "C" REX_FUNC(sub_833ADC98) {
  const uint32_t p = ctx.r3.u32, rq = ctx.r4.u32;
  if (FILE* o = Out(); o && Ok(p)) {
    // the wrapper's caller's frame is the back chain of this one; each frame's lr sits 8 below the next
    uint32_t chain[4] = {};
    uint32_t sp = L32(base, ctx.r1.u32);  // wrapper frame -> its caller's frame
    for (int k = 0; k < 4 && Ok(sp); ++k) {
      uint32_t up = L32(base, sp);
      if (!Ok(up) || up <= sp) break;
      chain[k] = L32(base, up - 8);
      sp = up;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    std::fprintf(o, "S %u %08x %08x %.3f %.3f %.3f %d %d %u %.2f %d %08x %08x %08x %08x\n", Tick(base), p,
                 t_outer ? t_outer : uint32_t(ctx.lr), LF(base, rq + 20), LF(base, rq + 24), LF(base, rq + 12),
                 int(L32(base, rq + 8)), int(L32(base, rq + 4)), unsigned(base[rq + 16]), LF(base, rq + 28),
                 int(L32(base, rq + 0)), chain[0], chain[1], chain[2], chain[3]);
  }
  __imp__sub_833ADC98(ctx, base);
}

// cpu puck carrier decisions:
//   BARDOWN_CARRIER_LOG=<dir> -> <dir>/carrier.txt, one line per carrier think (sub_83590C40) that reached
//   the option chooser (sub_83560CE8):
//   W/B lines once: runtime-set tweak values and the skate config block
//   K tick P idx team pos x z f1..f6 rec choice ret10 ready quality held reach ret st dkind sgain sloss dgain dloss bgain bloss rush v0..v23 | acts
//   f1..f6 = bias variant args (skate deke pass shoot dump board), rec = analysis record,
//   ret = rec+600, st = rec+592, dkind = rec+1172, v = the 24 option values after biases (chooser input),
//   acts = releases issued from the core this tick: name:r4:f1
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_83590C40);
REX_EXTERN(__imp__sub_834F5F98);
REX_EXTERN(__imp__sub_83560CE8);
REX_EXTERN(__imp__sub_83402788);
REX_EXTERN(__imp__sub_83562070);
REX_EXTERN(__imp__sub_83402628);
REX_EXTERN(__imp__sub_83562248);
REX_EXTERN(__imp__sub_83435830);
REX_EXTERN(__imp__sub_83402AF8);
REX_EXTERN(__imp__sub_83402610);
REX_EXTERN(__imp__sub_83415868);
REX_EXTERN(__imp__sub_83402790);
REX_EXTERN(__imp__sub_83314F00);
REX_EXTERN(__imp__sub_8355F748);
REX_EXTERN(__imp__sub_8355B420);

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

std::mutex g_mu;
FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_CARRIER_LOG");
    FILE* f = dir && *dir ? std::fopen((std::string(dir) + "/carrier.txt").c_str(), "w") : nullptr;
    if (f) std::setvbuf(f, nullptr, _IOFBF, 1 << 20);
    return f;
  }();
  return out;
}

// one core call at a time per thread
thread_local uint32_t t_core = 0, t_rec = 0;
thread_local bool t_have = false;
thread_local int t_choice = -1;
thread_local float t_val[24];
thread_local std::string t_acts;

bool FromCore(uint32_t lr) { return t_core && lr >= 0x83590C40u && lr < 0x835923F0u; }

void Act(const char* name, PPCContext& ctx) {
  if (!FromCore(uint32_t(ctx.lr))) return;
  char buf[96];
  std::snprintf(buf, sizeof buf, " %s:%08x:%d:%.3f", name, ctx.r4.u32, ctx.r5.s32, ctx.f1.f64);
  t_acts += buf;
}

}  // namespace

extern "C" REX_FUNC(sub_83590C40) {
  const uint32_t p = ctx.r3.u32;
  const float f[6] = {float(ctx.f1.f64), float(ctx.f2.f64), float(ctx.f3.f64),
                      float(ctx.f4.f64), float(ctx.f5.f64), float(ctx.f6.f64)};
  FILE* o = Out();
  if (!o) {
    __imp__sub_83590C40(ctx, base);
    return;
  }
  t_core = p;
  t_rec = 0;
  t_have = false;
  t_acts.clear();
  __imp__sub_83590C40(ctx, base);
  t_core = 0;
  if (!t_have || !Ok(p)) return;
  uint32_t body = L32(base, p + 32), info = L32(base, p + 192), r = t_rec;
  float x = Ok(body) ? LF(base, body + 48) : 0, z = Ok(body) ? LF(base, body + 56) : 0;
  std::lock_guard<std::mutex> lk(g_mu);
  static bool dumped = false;
  if (!dumped) {
    dumped = true;
    static const uint32_t cv[] = {0x83CC4468, 0x83CA6C0C, 0x83CDE1AC, 0x83CD00AC, 0x83CE0A2C, 0x83CD2F98,
                                  0x83CC8620, 0x83CC722C, 0x83CC3234, 0x83CA68E8, 0x83CC74A4};
    for (uint32_t a : cv) std::fprintf(o, "W %08x %g %08x\n", a, LF(base, a + 40), L32(base, a + 40));
    std::fprintf(o, "W step %g flag %u\nB", LF(base, 0x83BFA334), unsigned(base[0x83A9B1AC]));
    for (uint32_t k = 0; k < 0x234; k += 4) std::fprintf(o, " %08x", L32(base, 0x83C51850 + k));
    std::fprintf(o, "\n");
  }
  std::fprintf(o, "K %u %08x %d %d %u %.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f %08x %d", L32(base, 0x83C75CA0), p,
               int(int8_t(base[p + 57])), int(int8_t(base[p + 48])), Ok(info) ? unsigned(base[info + 41]) : 255u, x, z,
               f[0], f[1], f[2], f[3], f[4], f[5], r, t_choice);
  if (Ok(r))
    std::fprintf(o, " %.3f %.3f %.3f %d %d", LF(base, r + 596), LF(base, r + 972), LF(base, r + 412), int(L32(base, r + 420)),
                 int(L32(base, r + 612)));
  else
    std::fprintf(o, " 0 0 0 0 0");
  if (Ok(r))
    std::fprintf(o, " %.3f %d %d %.4f %.4f %.4f %.4f %.4f %.4f %d", LF(base, r + 600), int(L32(base, r + 592)),
                 int(L32(base, r + 1172)), LF(base, r + 540), LF(base, r + 544), LF(base, r + 572), LF(base, r + 576),
                 LF(base, r + 616), LF(base, r + 620), int(L32(base, r + 2148)));
  else
    std::fprintf(o, " 0 0 0 0 0 0 0 0 0 0");
  for (int k = 0; k < 24; ++k) std::fprintf(o, " %.4g", t_val[k]);
  std::fprintf(o, " |%s\n", t_acts.c_str());
  static uint32_t last_flush = 0;
  if (uint32_t now = L32(base, 0x83C75CA0); now >= last_flush + 30) {
    last_flush = now;
    std::fflush(o);
  }
}

extern "C" REX_FUNC(sub_834F5F98) {
  const uint32_t p = ctx.r3.u32;
  __imp__sub_834F5F98(ctx, base);
  if (t_core && p == t_core && !t_rec) t_rec = ctx.r3.u32;
}

extern "C" REX_FUNC(sub_83560CE8) {
  const uint32_t v = ctx.r4.u32;
  const bool mine = FromCore(uint32_t(ctx.lr));
  if (mine)
    for (int k = 0; k < 24; ++k) t_val[k] = LF(base, v + 4 * k);
  __imp__sub_83560CE8(ctx, base);
  if (mine) {
    t_have = true;
    t_choice = int(ctx.r3.s32);
  }
}

#define ACT_HOOK(addr, name)                        \
  extern "C" REX_FUNC(sub_##addr) {                 \
    Act(name, ctx);                                 \
    __imp__sub_##addr(ctx, base);                   \
  }

ACT_HOOK(83402788, "shot")
ACT_HOOK(83562070, "shotdelay")
ACT_HOOK(83402628, "shot1t")
ACT_HOOK(83562248, "passquick")
ACT_HOOK(83435830, "pass")
ACT_HOOK(83402AF8, "dumphard")
ACT_HOOK(83402610, "dumpsoft")
ACT_HOOK(83415868, "dumpslap")
ACT_HOOK(83402790, "dumpother")
ACT_HOOK(83314F00, "board")
ACT_HOOK(8355F748, "deke")
ACT_HOOK(8355B420, "push")

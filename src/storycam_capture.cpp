// code cameras at a stoppage (storyteller, pre-faceoff, gameplay faceoff zoom), logged:
//   BARDOWN_CAM_LOG=<dir> -> <dir>/cam.txt
//   SET <ms> <tick> <type> <player> <r6> <r7> <lr> | ctx <story ctx words 0..11>   camsys SetCamera (sub_8346D840)
//   GEN <ms> <tick> <obj> <type> f192 f352 f356 f360 f364 f376 f380 f388 f392 | dir ...   CamGeneric built
//   GU <ms> <obj> <type> t368 n384 b256 pos16 v32 f48 v336 | subj <idx> <x y z>   CamGeneric update (after)
//   STA <ms> <tick> <obj> <type> <dwords 52..160>                                   Static built
//   SU <ms> <obj> <type> pos16 v32 f48 | words 64..156
//   VIEW <ms> <tick> <sitcom> <fomgr> <camtype> <player> <eye xyz> <ang xyz> <e>    what the scene view used
//   P <ms> <tick> <idx> <kind> <x y z>   every player, every 10th VIEW
// ms = host ms since load, tick = 0x83C75BF8+168

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_8346D840);
REX_EXTERN(__imp__sub_83553918);
REX_EXTERN(__imp__sub_83466E58);
REX_EXTERN(__imp__sub_834675F8);
REX_EXTERN(__imp__sub_83459080);
REX_EXTERN(__imp__sub_83459220);
REX_EXTERN(__imp__sub_8345AFA0);
REX_EXTERN(__imp__sub_8345AD90);

namespace {

const auto g_start = std::chrono::steady_clock::now();
constexpr uint32_t kDir = 0x83C80330;  // camera director state (*(0x83C7F038))

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

std::recursive_mutex g_lock;

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_CAM_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/cam.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

long Ms() {
  return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
}
uint32_t Tick(const uint8_t* b) { return L32(b, 0x83C75BF8 + 168); }

void V3(FILE* o, const uint8_t* b, uint32_t a) {
  std::fprintf(o, " %.4f %.4f %.4f", LF(b, a), LF(b, a + 4), LF(b, a + 8));
}

void Subject(FILE* o, const uint8_t* b, int32_t idx) {
  std::fprintf(o, " | subj %d", idx);
  if (idx < 0 || idx >= 64) return;
  const uint32_t p = L32(b, 0x83BFD1F0 + 4 * idx);
  if (!Ok(p)) return;
  const uint32_t body = L32(b, p + 32);
  if (!Ok(body)) return;
  std::fprintf(o, " kind %u", L32(b, p + 40));
  V3(o, b, body + 48);
}

int32_t g_story_time = -1;
float g_eye[3], g_ang[3];
uint32_t g_views = 0;

}  // namespace

// camsys SetCamera(this, type, player, r6, r7)
extern "C" REX_FUNC(sub_8346D840) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "SET %ld %u st %d %d %d %d %d %08x | cur %d %d | ctx", Ms(), Tick(base), g_story_time, int32_t(ctx.r4.u32),
                 int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), int32_t(ctx.r7.u32), uint32_t(ctx.lr),
                 int32_t(L32(base, ctx.r3.u32 + 416)), int32_t(L32(base, ctx.r3.u32 + 536)));
    for (int i = 0; i < 32; ++i) std::fprintf(o, " %d", int32_t(L32(base, 0x83CBE5C8 + 4 * i)));
    std::fprintf(o, "\n");
  }
  const uint32_t cs = ctx.r3.u32;
  const int32_t who = int32_t(ctx.r5.u32);
  __imp__sub_8346D840(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    uint32_t team = 0xFF;
    if (who >= 0 && who < 40)
      if (const uint32_t p = L32(base, 0x83BFD150 + 4 * who); Ok(p)) team = base[p + 48];
    std::fprintf(o, "SIDE %.1f team %u\n", LF(base, cs + 540), team);
  }
}

extern "C" REX_FUNC(sub_83466E58) {
  const uint32_t obj = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_83466E58(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "GEN %ld %u %08x %d", Ms(), Tick(base), obj, int32_t(type));
    std::fprintf(o, " %d %.4f %d %d %d %.4f %d %u %.4f", int32_t(L32(base, obj + 192)), LF(base, obj + 352),
                 int32_t(L32(base, obj + 356)), int32_t(L32(base, obj + 360)), int32_t(L32(base, obj + 364)),
                 LF(base, obj + 376), int32_t(L32(base, obj + 380)), base[obj + 388], LF(base, obj + 392));
    std::fprintf(o, " | dir 788=%d 832=%u 833=%u 836=%u 839=%u 841=%u 844=%d 852=%d 856=%d 860=%d 864=%d 868=%d 992=%d 1074=%u 1124=%.4f 1132=%d 1368=%.4f",
                 int32_t(L32(base, kDir + 788)), base[kDir + 832], base[kDir + 833], base[kDir + 836],
                 base[kDir + 839], base[kDir + 841], int32_t(L32(base, kDir + 844)), int32_t(L32(base, kDir + 852)),
                 int32_t(L32(base, kDir + 856)), int32_t(L32(base, kDir + 860)), int32_t(L32(base, kDir + 864)),
                 int32_t(L32(base, kDir + 868)), int32_t(L32(base, kDir + 992)), base[kDir + 1074],
                 LF(base, kDir + 1124), int32_t(L32(base, kDir + 1132)), LF(base, kDir + 1368));
    std::fprintf(o, " dt=%.5f fothr=%.4f fl458=%u/%u/%u", LF(base, kDir + 4), LF(base, 0x83CBFD3C), base[0x83C802AC + 0],
                 0u, 0u);
    std::fprintf(o, " 1376=");
    V3(o, base, kDir + 1376);
    std::fprintf(o, " 1408=");
    V3(o, base, kDir + 1408);
    std::fprintf(o, " | tw");
    for (uint32_t a = 0x83AB3A40; a <= 0x83AB3A80; a += 4) std::fprintf(o, " %.5g", LF(base, a));
    std::fprintf(o, " %d", int32_t(L32(base, 0x83AB25F4)));
    if (const uint32_t pv = L32(base, 0x83C802AC); Ok(pv) && Ok(L32(base, pv)))
      std::fprintf(o, " prov %08x +24 %08x +28 %08x +36 %08x", pv, L32(base, L32(base, pv) + 24),
                   L32(base, L32(base, pv) + 28), L32(base, L32(base, pv) + 36));
    Subject(o, base, int32_t(L32(base, obj + 380)));
    std::fprintf(o, "\n");
  }
}

extern "C" REX_FUNC(sub_834675F8) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_834675F8(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "GU %ld %08x %d %.4f %d %u", Ms(), obj, int32_t(L32(base, obj + 52)), LF(base, obj + 368),
                 int32_t(L32(base, obj + 384)), base[obj + 256]);
    V3(o, base, obj + 16);
    V3(o, base, obj + 32);
    std::fprintf(o, " %.5f", LF(base, obj + 48));
    V3(o, base, obj + 336);
    std::fprintf(o, " dof %u %u %.4f %.4f %.4f", base[kDir + 1332], base[kDir + 1333], LF(base, kDir + 1336),
                 LF(base, kDir + 1340), LF(base, kDir + 1344));
    Subject(o, base, int32_t(L32(base, obj + 380)));
    std::fprintf(o, "\n");
  }
}

extern "C" REX_FUNC(sub_83459080) {
  const uint32_t obj = ctx.r3.u32, type = ctx.r4.u32;
  __imp__sub_83459080(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "STA %ld %u %08x %d", Ms(), Tick(base), obj, int32_t(type));
    for (uint32_t k = 52; k < 160; k += 4) std::fprintf(o, " %08x", L32(base, obj + k));
    std::fprintf(o, "\n");
  }
}

extern "C" REX_FUNC(sub_83459220) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_83459220(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "SU %ld %08x %d", Ms(), obj, int32_t(L32(base, obj + 52)));
    V3(o, base, obj + 16);
    V3(o, base, obj + 32);
    std::fprintf(o, " %.5f |", LF(base, obj + 48));
    for (uint32_t k = 64; k < 160; k += 4) std::fprintf(o, " %.4g", LF(base, obj + k));
    std::fprintf(o, "\n");
  }
}

// the scene view (sub_8364FF60) asks camsys for eye (+248), angles (+252) and fov (+480)
extern "C" REX_FUNC(sub_8345AFA0) {
  const uint32_t out = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8345AFA0(ctx, base);
  if (lr >= 0x8364FF60u && lr < 0x83650800u && Ok(out)) {
    std::lock_guard hold(g_lock);
    for (int i = 0; i < 3; ++i) g_eye[i] = LF(base, out + 4 * i);
  }
}
extern "C" REX_FUNC(sub_8345AD90) {
  const uint32_t out = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8345AD90(ctx, base);
  if (lr >= 0x8364FF60u && lr < 0x83650800u && Ok(out)) {
    std::lock_guard hold(g_lock);
    for (int i = 0; i < 3; ++i) g_ang[i] = LF(base, out + 4 * i);
  }
}
// fov (+480 = sub_8343A560) is hooked in nis_capture.cpp, which calls this after it
void StorycamFov(const uint8_t* base, uint32_t cs, uint32_t lr, double fov) {
  if (FILE* o = Out(); o && lr >= 0x8364FF60u && lr < 0x83650800u && Ok(cs)) {
    std::lock_guard hold(g_lock);
    const long ms = Ms();
    const uint32_t tick = Tick(base);
    std::fprintf(o, "VIEW %ld %u %d %d %d %d %.4f %.4f %.4f %.5f %.5f %.5f %.6f\n", ms, tick,
                 int32_t(L32(base, 0x83BFD050)), int32_t(L32(base, 0x83C75C40)), int32_t(L32(base, cs + 416)),
                 int32_t(L32(base, cs + 536)), g_eye[0], g_eye[1], g_eye[2], g_ang[0], g_ang[1], g_ang[2], fov);
    if (++g_views % 10 == 0) {
      for (int i = 0; i < 40; ++i) {
        const uint32_t p = L32(base, 0x83BFD1F0 + 4 * i);
        if (!Ok(p)) continue;
        const uint32_t body = L32(base, p + 32);
        if (!Ok(body)) continue;
        std::fprintf(o, "P %ld %u %d %u", ms, tick, i, L32(base, p + 40));
        V3(o, base, body + 48);
        std::fprintf(o, "\n");
      }
    }
    if (g_views % 300 == 0) std::fflush(o);
  }
}

// intermediates of the CamGeneric update (lr inside sub_834675F8)
REX_EXTERN(__imp__sub_834391C0);
REX_EXTERN(__imp__sub_83441410);
REX_EXTERN(__imp__sub_83441870);

namespace {
bool InGenericUpdate(uint64_t lr) { return lr >= 0x834675F8u && lr < 0x83467BC0u; }
}  // namespace

// preset blend: r3 out, r4 preset A, r5 preset B, r6 count, f1 weight
extern "C" REX_FUNC(sub_834391C0) {
  const uint32_t out = ctx.r3.u32, n = ctx.r6.u32;
  const double w = ctx.f1.f64;
  const uint64_t lr = ctx.lr;
  __imp__sub_834391C0(ctx, base);
  if (FILE* o = Out(); o && InGenericUpdate(lr) && Ok(out)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "GB %.4f", w);
    for (uint32_t i = 0; i < n && i < 16; ++i) std::fprintf(o, " %.4f", LF(base, out + 4 * i));
    std::fprintf(o, "\n");
  }
}

// subject transform: r3 out (4 rows), r4 player
extern "C" REX_FUNC(sub_83441870) {
  const uint32_t out = ctx.r3.u32, who = ctx.r4.u32;
  const uint64_t lr = ctx.lr;
  __imp__sub_83441870(ctx, base);
  if (FILE* o = Out(); o && InGenericUpdate(lr) && Ok(out)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "GM %d", int32_t(who));
    for (int r = 0; r < 4; ++r) V3(o, base, out + 16 * r);
    std::fprintf(o, "\n");
  }
}

// rig solve: r3 out target, r4 rig, r5 point, r6 point2, r7 flag, r8 preset copy (7 floats), r9, f1
extern "C" REX_FUNC(sub_83441410) {
  const uint32_t out = ctx.r3.u32, rig = ctx.r4.u32, p5 = ctx.r5.u32, p6 = ctx.r6.u32, p8 = ctx.r8.u32;
  const double f1 = ctx.f1.f64;
  const uint64_t lr = ctx.lr;
  float in5[3], in6[3];
  for (int i = 0; i < 3; ++i) in5[i] = LF(base, p5 + 4 * i), in6[i] = LF(base, p6 + 4 * i);
  __imp__sub_83441410(ctx, base);
  if (FILE* o = Out(); o && InGenericUpdate(lr) && Ok(out)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "GR %.4f in5 %.4f %.4f %.4f in6 %.4f %.4f %.4f pre", f1, in5[0], in5[1], in5[2], in6[0], in6[1],
                 in6[2]);
    for (int i = 0; i < 7; ++i) std::fprintf(o, " %.4f", LF(base, p8 + 4 * i));
    std::fprintf(o, " out");
    V3(o, base, out);
    std::fprintf(o, " rig");
    V3(o, base, rig);
    std::fprintf(o, "\n");
  }
}

// storyteller update (ctx, time): remember the time it was handed
extern "C" REX_FUNC(sub_83553918) {
  g_story_time = int32_t(ctx.r4.u32);
  __imp__sub_83553918(ctx, base);
}

// what makes the game's sounds, for matching its sound banks to events:
//   BARDOWN_SOUND_LOG=<dir> scripts/run.sh match
// every closing physics contact (sub_835500F8) goes to <dir>/contacts.txt as
//   steady-clock ns, material A, material B, closing speed vn, contact point x y z
// and every shot and pass launch (sub_834EAFE8, sub_834EB0B0) and every hit on a skater
// (sub_83320E68: 3/4/5 stumble and knock-down from checks, 15 nudge, 13/14 stick, ...) to
// <dir>/launches.txt as
//   steady-clock ns, shot|pass|hit, speed or hit type
// on the same clock as the XMA input buffers the runtime logs to <dir>/xma.txt (sdk/sound-log.patch).
// the materials come from each shape's own lookup (vtable +12 with the sub-shape index), as the
// function does itself; the registers it clobbers are put back before the real call

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_835500F8);
REX_EXTERN(__imp__sub_834EAFE8);
REX_EXTERN(__imp__sub_834EB0B0);
REX_EXTERN(__imp__sub_83320E68);

namespace {

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_SOUND_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/contacts.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

FILE* Launches() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_SOUND_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/launches.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

long long Now() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void Launch(const char* what, double speed) {
  static std::mutex mutex;
  if (FILE* f = Launches()) {
    std::lock_guard lock(mutex);
    std::fprintf(f, "%lld %s %.3f\n", Now(), what, speed);
    std::fflush(f);
  }
}

uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

float LoadF(const uint8_t* base, uint32_t addr) {
  uint32_t v = Load32(base, addr);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

uint32_t Material(PPCContext& ctx, uint8_t* base, uint32_t shape, uint32_t index) {
  if (!shape) return 0xFFFFFFFF;
  uint32_t fn = Load32(base, Load32(base, shape) + 12);
  ctx.r3.u64 = shape;
  ctx.r4.u64 = index;
  rex::runtime::ResolveIndirectFunction(fn)(ctx, base);
  return ctx.r3.u32;
}

}  // namespace

extern "C" REX_FUNC(sub_835500F8) {
  FILE* out = Out();
  const uint32_t rec = ctx.r4.u32;
  if (out && LoadF(base, rec + 84) < 0) {
    PPCContext saved = ctx;
    uint32_t a = Material(ctx, base, Load32(base, rec + 8), Load32(base, rec + 92));
    uint32_t b = Material(ctx, base, Load32(base, rec + 12), Load32(base, rec + 96));
    ctx = saved;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::fprintf(out, "%lld %u %u %.4f %.3f %.3f %.3f\n", static_cast<long long>(ns), a, b, LoadF(base, rec + 84),
                 LoadF(base, rec + 32), LoadF(base, rec + 36), LoadF(base, rec + 40));
    std::fflush(out);
  }
  __imp__sub_835500F8(ctx, base);
}

extern "C" REX_FUNC(sub_834EAFE8) {
  Launch("shot", ctx.f1.f64);
  __imp__sub_834EAFE8(ctx, base);
}

extern "C" REX_FUNC(sub_834EB0B0) {
  Launch("pass", ctx.f1.f64);
  __imp__sub_834EB0B0(ctx, base);
}

extern "C" REX_FUNC(sub_83320E68) {
  Launch("hit", double(ctx.r4.u32));
  __imp__sub_83320E68(ctx, base);
}

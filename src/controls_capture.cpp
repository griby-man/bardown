// pad -> action layer: what each control does:
//   BARDOWN_CTRL_LOG=<dir> -> <dir>/ctrl.txt
//   ST <ms> <tick> <ctrl> <type> | <state +20..+83 as u16 words>     NHLController pad state, on change
//   JP <ms> <tick> <ctrl> <action> <mask> <which> <lr>                 just-pressed query true (sub_834091B8)
//   JR <ms> <tick> <ctrl> <action> <mask> <which> <lr>                 just-released query true (sub_83409130)
//   BARDOWN_FORCE_SCHEME=<0..3> points the controller at that scheme's table and sets the slot's type,
//   the same pick sub_835F6B88 makes (1 -> table +168, 2 -> +328, else +8)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_834091B8);
REX_EXTERN(__imp__sub_83409130);
REX_EXTERN(__imp__sub_82D29028);
REX_EXTERN(__imp__sub_83410670);

namespace {

const auto g_start = std::chrono::steady_clock::now();

uint32_t L32(const uint8_t* b, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, b + a, 4);
  return __builtin_bswap32(v);
}
uint16_t L16(const uint8_t* b, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, b + a, 2);
  return __builtin_bswap16(v);
}
void S32(uint8_t* b, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(b + a, &v, 4);
}

std::mutex g_lock;

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_CTRL_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/ctrl.txt").c_str(), "w") : nullptr;
  }();
  return out;
}
int Forced() {
  static int f = std::getenv("BARDOWN_FORCE_SCHEME") ? std::atoi(std::getenv("BARDOWN_FORCE_SCHEME")) : -1;
  return f;
}
long Ms() {
  return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
}
uint32_t Tick(const uint8_t* b) { return L32(b, 0x83C75BF8 + 168); }

// slot config (+140) of a controller, as sub_835F6B88 finds it
uint32_t SlotConfig(const uint8_t* b, uint32_t ctrl) {
  const uint32_t sys = L32(b, 0x83CF1688);
  if (!sys) return 0;
  const int32_t id = int32_t(L32(b, ctrl + 84));
  const uint32_t idx = id == -1 ? 0 : uint32_t(((id >> 8) * 8) + (id & 0xFF));
  return sys + idx * 488 + 140;
}

// every registered tweak once: TW <obj> <vtable> <category> <name> | +28 +32 +40 +44 (hex) | +40 as float
// (list at 0x83D6C490: next +20, category +4, name +8)
void DumpTweaks(const uint8_t* b) {
  const char* dir = std::getenv("BARDOWN_CTRL_LOG");
  if (!dir) return;
  FILE* o = std::fopen((std::string(dir) + "/tweaks.txt").c_str(), "w");
  if (!o) return;
  int n = 0;
  for (uint32_t t = L32(b, 0x83D6C490); t && n < 100000; t = L32(b, t + 20), ++n) {
    auto str = [&](uint32_t a) { return a ? reinterpret_cast<const char*>(b + a) : ""; };
    uint32_t w40 = L32(b, t + 40);
    float f40;
    std::memcpy(&f40, &w40, 4);
    std::fprintf(o, "TW %08x %08x %s %s | %08x %08x %08x %08x | %g\n", t, L32(b, t), str(L32(b, t + 4)),
                 str(L32(b, t + 8)), L32(b, t + 28), L32(b, t + 32), w40, L32(b, t + 44), f40);
  }
  // BARDOWN_CTRL_PEEK=<hex addr>:<hex bytes>,...: those ranges as words and floats
  if (const char* pk = std::getenv("BARDOWN_CTRL_PEEK")) {
    std::string list = pk;
    for (size_t at = 0; at < list.size();) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      uint32_t a = 0, len = 0;
      std::sscanf(list.substr(at, comma - at).c_str(), "%x:%x", &a, &len);
      for (uint32_t k = 0; k < len; k += 4) {
        uint32_t w = L32(b, a + k);
        float f;
        std::memcpy(&f, &w, 4);
        std::fprintf(o, "PK %08x %08x %g\n", a + k, w, f);
      }
      at = comma + 1;
    }
  }
  std::fclose(o);
}

void Before(uint8_t* b, uint32_t ctrl) {
  static bool dumped = false;
  if (!dumped && Tick(b) > 600) {
    dumped = true;
    DumpTweaks(b);
  }
  if (Forced() >= 0) {
    if (const uint32_t cfg = SlotConfig(b, ctrl)) {
      const int s = Forced();
      S32(b, cfg + 4, uint32_t(s));
      S32(b, ctrl + 12, cfg + (s == 1 ? 168 : s == 2 ? 328 : 8));
    }
  }
  FILE* o = Out();
  if (!o) return;
  static std::unordered_map<uint32_t, std::string> last;
  std::string cur(reinterpret_cast<const char*>(b + ctrl + 20), 64);
  auto& prev = last[ctrl];
  if (prev == cur) return;
  prev = cur;
  const uint32_t cfg = SlotConfig(b, ctrl);
  std::fprintf(o, "ST %ld %u %08x %d |", Ms(), Tick(b), ctrl, cfg ? int32_t(L32(b, cfg + 4)) : -9);
  for (uint32_t i = 0; i < 64; i += 2) std::fprintf(o, " %04x", L16(b, ctrl + 20 + i));
  std::fprintf(o, "\n");
}

bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

// " | hum <idx> <action> own <puck owner idx>": the player pad 0 drives (+556 < 0x80) and who has the puck
void Context(FILE* o, const uint8_t* b) {
  int hum = -1, act = -1;
  uint32_t hp = 0;
  for (int i = 0; i < 64; ++i) {
    const uint32_t p = L32(b, 0x83BFD1F0 + 4 * i);
    if (!Ok(p)) continue;
    if (b[p + 556] < 0x80) {
      hum = b[p + 57];
      act = int32_t(L32(b, p + 60));
      hp = p;
      break;
    }
  }
  int own = -1;
  if (const uint32_t pm = L32(b, 0x83BFD14C); Ok(pm)) own = int32_t(L32(b, pm + 180));
  // puck: 1 = the human player has it (owner index = his +57 index), 0 = someone else, -1 = loose
  std::fprintf(o, " | hum %d %d own %d puck %d %08x", hum, act, own, own < 0 ? -1 : int(own == hum), hp);
}

void Event(const char* tag, const uint8_t* b, uint32_t ctrl, uint32_t action, int32_t which, uint32_t lr) {
  FILE* o = Out();
  if (!o) return;
  const uint32_t table = L32(b, ctrl + 12);
  std::fprintf(o, "%s %ld %u %08x %u %u %d %08x", tag, Ms(), Tick(b), ctrl, action, L16(b, table + 4 + 2 * action),
               which, lr);
  Context(o, b);
  std::fprintf(o, "\n");
}

}  // namespace

// JustPressed(ctrl, action, which)
extern "C" REX_FUNC(sub_834091B8) {
  const uint32_t ctrl = ctx.r3.u32, action = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const int32_t which = int32_t(ctx.r5.u32);
  {
    std::lock_guard hold(g_lock);
    Before(base, ctrl);
  }
  __imp__sub_834091B8(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    std::lock_guard hold(g_lock);
    Event("JP", base, ctrl, action, which, lr);
  }
}

// JustReleased(ctrl, action, which)
extern "C" REX_FUNC(sub_83409130) {
  const uint32_t ctrl = ctx.r3.u32, action = ctx.r4.u32, lr = uint32_t(ctx.lr);
  const int32_t which = int32_t(ctx.r5.u32);
  {
    std::lock_guard hold(g_lock);
    Before(base, ctrl);
  }
  __imp__sub_83409130(ctx, base);
  if (ctx.r3.u32 & 0xFF) {
    std::lock_guard hold(g_lock);
    Event("JR", base, ctrl, action, which, lr);
  }
}

// gesture matcher's stick polar step (called by the analog stick matcher sub_82D293C8): r3 current
// (x, y, angle, magnitude), r4 previous, f1 OffsetAngle. logs AS <tick> x y angle mag | offset, once per
// distinct current vector with magnitude over 0.3, to pin the angle convention the gestures use
extern "C" REX_FUNC(sub_82D29028) {
  if (FILE* o = Out()) {
    float v[4];
    for (int i = 0; i < 4; ++i) {
      uint32_t w = L32(base, ctx.r3.u32 + 4 * i);
      std::memcpy(&v[i], &w, 4);
    }
    static float last[4];
    std::lock_guard hold(g_lock);
    if (v[3] > 0.3f && std::memcmp(v, last, sizeof v) != 0) {
      std::memcpy(last, v, sizeof v);
      const uint32_t frame = L32(base, ctx.r1.u32 + 84);
      std::fprintf(o, "AS %ld %u %.4f %.4f %.4f %.4f | %.4f opt %u%u%u mirror %d\n", Ms(), Tick(base), v[0], v[1], v[2],
                   v[3], ctx.f1.f64, ctx.r8.u32 & 1, ctx.r9.u32 & 1, ctx.r10.u32 & 1,
                   frame >= 0x40000000u && frame < 0xE0000000u ? base[frame + 12] : -1);
    }
  }
  __imp__sub_82D29028(ctx, base);
}

// RS direction check sub_83410670(player, relative, &target angle, &tolerance, ?, ?, &diff out; f1 min
// magnitude): true when the right stick is at least f1 deflected and within tolerance of the target.
// RG <ms> <tick> <lr> rel target tol minmag -> result, whenever a call site's result flips
extern "C" REX_FUNC(sub_83410670) {
  const uint32_t lr = uint32_t(ctx.lr), rel = ctx.r4.u32;
  auto f = [&](uint32_t a) {
    uint32_t w = a ? L32(base, a) : 0;
    float v;
    std::memcpy(&v, &w, 4);
    return v;
  };
  const float target = f(ctx.r5.u32), tol = f(ctx.r6.u32), minmag = float(ctx.f1.f64);
  __imp__sub_83410670(ctx, base);
  if (FILE* o = Out()) {
    static std::unordered_map<uint32_t, uint32_t> last;
    std::lock_guard hold(g_lock);
    const uint32_t r = ctx.r3.u32 & 0xFF;
    auto it = last.find(lr);
    if (it == last.end() || it->second != r) {
      last[lr] = r;
      std::fprintf(o, "RG %ld %u %08x %u %.4f %.4f %.4f -> %u\n", Ms(), Tick(base), lr, rel, target, tol, minmag, r);
    }
  }
}

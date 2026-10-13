// how a cinematic plays, logged:
//   BARDOWN_NIS_LOG=<dir> scripts/run.sh match
// <dir>/nis.txt, one line per event, all from the NIS scripts manager:
//   N <frame> <node offset> <op> <a> <b> <args...>   a script node run (sub_83469E48), offset in words
//                                                    from the start of scripts.bin's swapped copy
//   V <frame> <op> <result>                          a condition node's value (sub_8345E180)
//   P <frame> <op> <asset> <b> <start> <end> <cam>   a play node (sub_83474B30, op 2 or 3)
//   S <frame> <shot> <asset> <name> <start> <end> <time> <cam> <main> <flags464> <dt> <ctrl> <ctrl asset>
//     <pos xyz> <quat xyzw> <look xyz> <float> <posoff xyz> <lookoff xyz>   the shot camera (sub_8345C310)
//   C <frame> <cam obj> <main> <pos xyzw> <angles xyzw> <fov>             what reaches the camera (sub_8345ACC0)
//   L <frame> <cam obj> <look xyzw>                                       (sub_83469720)
//   F <frame> <cam obj> <idx> <caller lr> <result>                         camera half fov getter (sub_8343A560)
//   E <frame> <event> <team> <r6> <r7> then A <frame> <assignment table words 0..149>   script start (sub_834706F8)
//   B <frame> <shot> <player> <player index> <tag assignment> <actor asset> <x y z facing> <flag> <shot flags>
//                                                    an actor bound to a player (sub_83474590)
//   G <frame> <character> <actor asset> <x y z facing> <flag>             placement handed to the character (sub_8334F1C8)
//   T <frame> <seconds since start>                  every 30 manager frames, for lining up with the screenshots
// C lines also carry the caller's lr; S lines are followed by one K line per bound player:
//   K <frame> <player> <index> <body x y z>
// frame = calls of the manager update sub_8345E4E8 so far
//   R <frame> <lr> <x extent> <y extent> <near> <far>   the projection the scene view builds (sub_82E20E00
//                                                    called from sub_8364FF60): x extent = aspect * y extent
// BARDOWN_NIS_NOSKIP=1 makes every cinematic unskippable (sub_8345DE20 answers no), so the scripted
// pad presses don't cut them short

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_83469E48);
REX_EXTERN(__imp__sub_8345E180);
REX_EXTERN(__imp__sub_83474B30);
REX_EXTERN(__imp__sub_8345C310);
REX_EXTERN(__imp__sub_8345ACC0);
REX_EXTERN(__imp__sub_83469720);
REX_EXTERN(__imp__sub_8345E4E8);
REX_EXTERN(__imp__sub_8343A560);
REX_EXTERN(__imp__sub_834706F8);
REX_EXTERN(__imp__sub_83474590);
REX_EXTERN(__imp__sub_8334F1C8);
REX_EXTERN(__imp__sub_8345DE20);
REX_EXTERN(__imp__sub_82E20E00);

namespace {

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
uint16_t L16(const uint8_t* base, uint32_t a) {
  uint16_t v;
  std::memcpy(&v, base + a, 2);
  return __builtin_bswap16(v);
}
float LF(const uint8_t* base, uint32_t a) {
  uint32_t v = L32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

std::mutex g_lock;
uint32_t g_frame = 0;
uint32_t g_scripts = 0;  // the manager's swapped scripts copy (+4)

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_NIS_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/nis.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

std::string Str(const uint8_t* base, uint32_t a) {
  std::string s;
  if (!Ok(a)) return "-";
  for (int i = 0; i < 80; ++i) {
    char c = char(base[a + i]);
    if (!c) break;
    s += c;
  }
  return s.empty() ? "-" : s;
}

}  // namespace

extern "C" REX_FUNC(sub_8345E4E8) {
  {
    std::lock_guard hold(g_lock);
    ++g_frame;
    if (Ok(ctx.r3.u32)) g_scripts = L32(base, ctx.r3.u32 + 4);
    static const auto t0 = std::chrono::steady_clock::now();
    if (FILE* o = Out(); o && g_frame % 30 == 0) {
      std::fprintf(o, "T %u %.3f\n", g_frame,
                   std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
      std::fflush(o);
    }
  }
  __imp__sub_8345E4E8(ctx, base);
}

extern "C" REX_FUNC(sub_83469E48) {
  const uint32_t node = ctx.r4.u32;
  if (FILE* o = Out(); o && Ok(node)) {
    std::lock_guard hold(g_lock);
    uint16_t h = L16(base, node);
    std::fprintf(o, "N %u %d %u", g_frame, g_scripts ? int(node - g_scripts) / 2 : -1, h & 0x1FFFu);
    for (int i = 0; i < 12; ++i) std::fprintf(o, " %04x", L16(base, node + 2 * i));
    std::fprintf(o, "\n");
  }
  __imp__sub_83469E48(ctx, base);
}

extern "C" REX_FUNC(sub_8345E180) {
  const uint32_t n = ctx.r4.u32;
  const uint16_t op = Ok(n) ? L16(base, n + 6) : 0xFFFF;
  __imp__sub_8345E180(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "V %u %u %d\n", g_frame, op, int32_t(ctx.r3.u32));
  }
}

extern "C" REX_FUNC(sub_83474B30) {
  const uint32_t n = ctx.r4.u32;
  if (FILE* o = Out(); o && Ok(n)) {
    std::lock_guard hold(g_lock);
    const uint16_t op = L16(base, n + 6);
    const uint32_t args = L32(base, n + 8);
    std::fprintf(o, "P %u %u %u %u", g_frame, op, L16(base, args), L16(base, n + 2));
    for (int i = 1; i < 4; ++i) std::fprintf(o, " %u", L16(base, args + 2 * i));
    std::fprintf(o, "\n");
  }
  __imp__sub_83474B30(ctx, base);
}

extern "C" REX_FUNC(sub_8345C310) {
  const uint32_t shot = ctx.r3.u32, slot = ctx.r5.u32, main_cam = ctx.r6.u32;
  const double dt = ctx.f1.f64;
  __imp__sub_8345C310(ctx, base);
  FILE* o = Out();
  if (!o || !Ok(shot) || !Ok(slot)) return;
  std::lock_guard hold(g_lock);
  const uint32_t ctrl = L32(base, slot + 4);
  std::fprintf(o, "S %u %08x %u %s %g %g %g %u %u %u %g %08x %08x", g_frame, shot, L32(base, shot + 4),
               Str(base, L32(base, shot + 20)).c_str(), LF(base, shot + 24), LF(base, shot + 28), LF(base, shot + 32),
               base[slot], main_cam & 0xFF, base[shot + 464], dt, ctrl, Ok(ctrl) ? L32(base, ctrl + 64) : 0);
  const uint32_t rig = L32(base, slot + 8);
  if (Ok(ctrl) && Ok(rig)) {
    const uint16_t off = L16(base, rig + 28);
    const uint32_t pose = off ? rig + off : 0;
    auto vec = [&](uint32_t field, int n) {
      const uint32_t a = pose + L32(base, slot + field);
      for (int i = 0; i < n; ++i) std::fprintf(o, " %.5f", LF(base, a + 4 * i));
    };
    if (pose) {
      vec(20, 3);
      vec(40, 4);
      vec(60, 3);
      vec(80, 1);
    }
    for (int i = 0; i < 3; ++i) std::fprintf(o, " %.5f", LF(base, slot + 96 + 4 * i));
    for (int i = 0; i < 3; ++i) std::fprintf(o, " %.5f", LF(base, slot + 112 + 4 * i));
  }
  std::fprintf(o, "\n");
  if (main_cam & 0xFF) {
    const uint32_t b = L32(base, shot + 408), e = L32(base, shot + 412);
    for (uint32_t a = b; Ok(a) && a < e && a < b + 4 * 64; a += 4) {
      const uint32_t pl = L32(base, a);
      if (!Ok(pl)) continue;
      const uint32_t body = L32(base, pl + 32);
      std::fprintf(o, "K %u %08x %d", g_frame, pl, int(int8_t(base[pl + 57])));
      if (Ok(body))
        for (int i = 0; i < 3; ++i) std::fprintf(o, " %.5f", LF(base, body + 48 + 4 * i));
      std::fprintf(o, "\n");
    }
  }
}

void StorycamFov(const uint8_t* base, uint32_t cs, uint32_t lr, double fov);
extern "C" REX_FUNC(sub_8343A560) {
  const uint32_t cam = ctx.r3.u32, idx = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8343A560(ctx, base);
  StorycamFov(base, cam, lr, ctx.f1.f64);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "F %u %08x %u %08x %.6f\n", g_frame, cam, idx, lr, ctx.f1.f64);
  }
}

extern "C" REX_FUNC(sub_834706F8) {
  const uint32_t ev = ctx.r4.u32, team = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32;
  __imp__sub_834706F8(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "E %u %d %d %d %d\nA %u", g_frame, int(ev), int(team), int(r6), int(r7), g_frame);
    for (uint32_t i = 0; i < 150; ++i) std::fprintf(o, " %d", int32_t(L32(base, 0x83BFD2A8 + 4 * i)));
    std::fprintf(o, "\n");
  }
}

extern "C" REX_FUNC(sub_83474590) {
  const uint32_t shot = ctx.r3.u32, pl = ctx.r4.u32, place = ctx.r5.u32, tag = ctx.r6.u32;
  if (FILE* o = Out(); o && Ok(shot) && Ok(pl) && Ok(place) && Ok(tag)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "B %u %08x %08x %d %d %08x %.5f %.5f %.5f %.5f %u %u\n", g_frame, shot, pl,
                 int(int8_t(base[pl + 57])), int32_t(L32(base, tag + 24)), L32(base, place), LF(base, place + 4),
                 LF(base, place + 8), LF(base, place + 12), LF(base, place + 16), base[place + 20], base[shot + 464]);
  }
  __imp__sub_83474590(ctx, base);
}

extern "C" REX_FUNC(sub_8334F1C8) {
  const uint32_t ch = ctx.r3.u32, place = ctx.r4.u32;
  if (FILE* o = Out(); o && Ok(place)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "G %u %08x %08x %.5f %.5f %.5f %.5f %u\n", g_frame, ch, L32(base, place), LF(base, place + 4),
                 LF(base, place + 8), LF(base, place + 12), LF(base, place + 16), base[place + 20]);
  }
  __imp__sub_8334F1C8(ctx, base);
}

extern "C" REX_FUNC(sub_8345ACC0) {
  if (FILE* o = Out(); o && Ok(ctx.r4.u32) && Ok(ctx.r5.u32)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "C %u %08x %u %08x", g_frame, ctx.r3.u32, ctx.r7.u32 & 0xFF, uint32_t(ctx.lr));
    for (int i = 0; i < 4; ++i) std::fprintf(o, " %.5f", LF(base, ctx.r4.u32 + 4 * i));
    for (int i = 0; i < 4; ++i) std::fprintf(o, " %.5f", LF(base, ctx.r5.u32 + 4 * i));
    std::fprintf(o, " %.6f\n", ctx.f1.f64);
  }
  __imp__sub_8345ACC0(ctx, base);
}

extern "C" REX_FUNC(sub_83469720) {
  if (FILE* o = Out(); o && Ok(ctx.r4.u32)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "L %u %08x", g_frame, ctx.r3.u32);
    for (int i = 0; i < 4; ++i) std::fprintf(o, " %.5f", LF(base, ctx.r4.u32 + 4 * i));
    std::fprintf(o, "\n");
  }
  __imp__sub_83469720(ctx, base);
}

extern "C" REX_FUNC(sub_8345DE20) {
  static const bool noskip = std::getenv("BARDOWN_NIS_NOSKIP") != nullptr;
  if (noskip) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8345DE20(ctx, base);
}

extern "C" REX_FUNC(sub_82E20E00) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (FILE* o = Out(); o && lr >= 0x8364FF60u && lr < 0x83651000u && Ok(ctx.r4.u32) && Ok(ctx.r5.u32)) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "R %u %08x %.6f %.6f %.6f %.6f\n", g_frame, lr, LF(base, ctx.r4.u32), LF(base, ctx.r5.u32),
                 Ok(ctx.r7.u32) ? LF(base, ctx.r7.u32) : 0.f, Ok(ctx.r8.u32) ? LF(base, ctx.r8.u32) : 0.f);
  }
  __imp__sub_82E20E00(ctx, base);
}

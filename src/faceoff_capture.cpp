// faceoff contest + gesture engine probes:
//   BARDOWN_FO_LOG=<dir> -> <dir>/fo.txt
//   BARDOWN_FO_PLAN=<scenario>,<scenario>,... one per faceoff, drives the pad from the faceoff manager state
//   (needs BARDOWN_INPUT set so the script pad exists). scenario = name[:ticks after state 4 began]
//   names: none fhhold bhhold fhdraw bhdraw fhlift bhlift fhtie bhtie push shoot middraw
// faceoff manager 0x83C75C04: +0/+4 puck heading per team, +8 speed, +12/+16 action per team,
// +20/+24 player index, +36 state start tick, +60 state

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_833E7F18);
REX_EXTERN(__imp__sub_833C1400);
REX_EXTERN(__imp__sub_83329990);
REX_EXTERN(__imp__sub_833D7148);
REX_EXTERN(__imp__sub_833C15B0);
REX_EXTERN(__imp__sub_833CB660);
REX_EXTERN(__imp__sub_82D284E8);
REX_EXTERN(__imp__sub_82D293C8);
REX_EXTERN(__imp__sub_82D25760);
REX_EXTERN(__imp__sub_83510C50);
REX_EXTERN(__imp__sub_8333E9F0);

namespace {

constexpr uint32_t kMgr = 0x83C75C04;

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

std::mutex g_lock;
std::atomic<const uint8_t*> g_base{nullptr};

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_FO_LOG");
    FILE* f = dir && *dir ? std::fopen((std::string(dir) + "/fo.txt").c_str(), "w") : nullptr;
    if (f) setvbuf(f, nullptr, _IOLBF, 0);
    return f;
  }();
  return out;
}
uint32_t Tick(const uint8_t* b) { return L32(b, 0x83C75BF8 + 168); }
uint32_t State(const uint8_t* b) { return L32(b, kMgr + 60); }

// per player: index team ctrl pos
void Who(FILE* o, const uint8_t* b, uint32_t p) {
  if (!Ok(p)) {
    std::fprintf(o, " [-]");
    return;
  }
  const uint32_t ctl = L32(b, p + 508);
  int h97 = 0, h98 = 0;
  if (Ok(ctl)) h97 = int8_t(b[ctl + 97]), h98 = int8_t(b[ctl + 98]);
  const uint32_t pd = L32(b, p + 192);
  const uint32_t body = L32(b, p + 32);
  std::fprintf(o, " [%08x vt %08x idx %d team %d ctrl %d pos %d hold %d/%d head %.4f]", p, L32(b, p), int8_t(b[p + 57]),
               int8_t(b[p + 48]), b[p + 556], Ok(pd) ? b[pd + 41] : -1, h97, h98,
               Ok(body) ? LF(b, body + 128) : 0.f);
}

// --- pad autopilot --------------------------------------------------------------------------------
struct Plan {
  std::string name;
  int at = 0;
};
std::vector<Plan> g_plans;
std::atomic<int> g_fo{-1};        // faceoff counter (state 1 entries)
std::atomic<uint32_t> g_t4{0};    // tick state 4 began
std::atomic<uint32_t> g_t5{0};    // tick state 5 began (0 = not yet)
std::atomic<uint32_t> g_state{0};

void LoadPlans() {
  static bool done = false;
  if (done) return;
  done = true;
  const char* p = std::getenv("BARDOWN_FO_PLAN");
  if (!p) return;
  std::string s = p;
  size_t at = 0;
  while (at <= s.size()) {
    size_t c = s.find(',', at);
    if (c == std::string::npos) c = s.size();
    std::string item = s.substr(at, c - at);
    if (!item.empty()) {
      Plan pl;
      size_t col = item.find(':');
      pl.name = item.substr(0, col);
      if (col != std::string::npos) pl.at = std::atoi(item.c_str() + col + 1);
      g_plans.push_back(pl);
    }
    at = c + 1;
  }
}

}  // namespace

extern "C" void bardown_pad_override(double* axes, uint16_t* buttons) {
  const uint8_t* b = g_base.load();
  if (!b) return;
  LoadPlans();
  const int fo = g_fo.load();
  if (fo < 0 || fo >= int(g_plans.size())) return;
  const uint32_t st = State(b), now = Tick(b);
  // live from state 2 until 60 ticks after the drop
  const uint32_t t5 = g_t5.load();
  if (st < 2 && t5 == 0) return;
  if (t5 && now > t5 + 60) return;
  const Plan& pl = g_plans[fo];
  // the 2->3->4 transitions happen outside the manager tick, so track them here
  static uint32_t last_st = 0, t3 = 0, t4 = 0;
  static int last_fo = -1;
  if (fo != last_fo) last_fo = fo, t3 = 0, t4 = 0, last_st = st;
  if (st != last_st) {
    if (st == 3) t3 = now;
    if (st == 4) t4 = now;
    last_st = st;
  }
  // at < 0: move once state 3 is -at-1 ticks old (before the glide), else ticks after state 4 began
  const bool moved = pl.at < 0 ? (t3 && int(now - t3) >= -pl.at - 1) || t4 || t5
                               : (t4 && int(now - t4) >= pl.at) || (t5 && !t4);
  auto set = [&](int i, double v) { axes[i] = v; };
  const std::string& n = pl.name;
  if (n == "fhhold") set(2, 1);
  else if (n == "bhhold") set(2, -1);
  else if (n == "fhdraw") moved ? set(3, -1) : set(2, 1);
  else if (n == "bhdraw") moved ? set(3, -1) : set(2, -1);
  else if (n == "fhlift") moved ? set(3, 1) : set(2, 1);
  else if (n == "bhlift") moved ? set(3, 1) : set(2, -1);
  else if (n == "fhtie") {
    set(2, 1);
    if (moved) set(1, 1);
  } else if (n == "bhtie") {
    set(2, -1);
    if (moved) set(1, 1);
  } else if (n == "push") {
    if (moved) set(3, 1), *buttons |= 0x0100;  // LB
    else set(2, 1);
  } else if (n == "shoot") {
    if (moved) set(3, 1);
  } else if (n == "middraw") {
    if (moved) set(3, -1);
  }
}

// faceoff manager tick
extern "C" REX_FUNC(sub_833E7F18) {
  g_base = base;
  const uint32_t before = State(base);
  __imp__sub_833E7F18(ctx, base);
  const uint32_t after = State(base);
  if (after != before) {
    const uint32_t now = Tick(base);
    if (after == 1) ++g_fo;
    if (after == 1 || after == 2) g_t4 = 0, g_t5 = 0;
    if (after == 4) g_t4 = now;
    if (after == 5) g_t5 = now;
    g_state = after;
    if (FILE* o = Out()) {
      std::lock_guard hold(g_lock);
      LoadPlans();
      const int fo = g_fo.load();
      std::fprintf(o, "S %u fo %d %u -> %u plan %s:%d | act %d %d\n", now, fo, before, after,
                   fo >= 0 && fo < int(g_plans.size()) ? g_plans[fo].name.c_str() : "-",
                   fo >= 0 && fo < int(g_plans.size()) ? g_plans[fo].at : 0, int32_t(L32(base, kMgr + 12)),
                   int32_t(L32(base, kMgr + 16)));
    }
  }
}

// SetAction(mgr, player, act)
extern "C" REX_FUNC(sub_833C1400) {
  const uint32_t p = ctx.r4.u32, act = ctx.r5.u32;
  const int team = Ok(p) ? int8_t(base[p + 48]) : -1;
  const int32_t pre = team >= 0 ? int32_t(L32(base, kMgr + 12 + 4 * team)) : -9;
  __imp__sub_833C1400(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    const int32_t post = team >= 0 ? int32_t(L32(base, kMgr + 12 + 4 * team)) : -9;
    std::fprintf(o, "A %u st %u act %u pre %d post %d lr %08x", Tick(base), State(base), act, pre, post,
                 uint32_t(ctx.lr));
    Who(o, base, p);
    std::fprintf(o, "\n");
  }
}

// anim query(anim, q) -> bool: the faceoff move states 78..88, 124, 125
extern "C" REX_FUNC(sub_83329990) {
  const uint32_t anim = ctx.r3.u32, q = ctx.r4.u32;
  __imp__sub_83329990(ctx, base);
  if ((q >= 78 && q <= 88) || q == 124 || q == 125) {
    if (FILE* o = Out()) {
      const uint32_t r = ctx.r3.u32 & 0xFF;
      std::lock_guard hold(g_lock);
      static std::map<uint64_t, uint32_t> last;
      const uint64_t key = (uint64_t(anim) << 32) | q;
      auto it = last.find(key);
      if (it == last.end() ? r != 0 : it->second != r) {
        last[key] = r;
        std::fprintf(o, "Q %u st %u anim %08x q %u -> %u\n", Tick(base), State(base), anim, q, r);
      }
    }
  }
}

// scores(mgr, A, B, &sA, &sB)
extern "C" REX_FUNC(sub_833C15B0) {
  const uint32_t pa = ctx.r6.u32, pb = ctx.r7.u32;
  __imp__sub_833C15B0(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "SC %u sA %.5f sB %.5f\n", Tick(base), LF(base, pa), LF(base, pb));
  }
}

// winner = resolve(mgr, A, B)
extern "C" REX_FUNC(sub_833D7148) {
  const uint32_t a = ctx.r4.u32, bb = ctx.r5.u32;
  const int32_t aa = int32_t(L32(base, kMgr + 12)), ab = int32_t(L32(base, kMgr + 16));
  __imp__sub_833D7148(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    const uint32_t settings = L32(base, 0x83BFD04C);
    std::fprintf(o, "R %u act %d %d winner %08x set0 %d set312 %d |", Tick(base), aa, ab, ctx.r3.u32,
                 Ok(settings) ? int32_t(L32(base, settings)) : -9,
                 Ok(settings) ? int32_t(L32(base, settings + 312)) : -9);
    Who(o, base, a);
    Who(o, base, bb);
    std::fprintf(o, "\n");
    // raw player objects, once per index, to find the handedness byte
    static std::set<int> dumped;
    for (uint32_t p : {a, bb}) {
      if (!Ok(p) || !dumped.insert(int8_t(base[p + 57])).second) continue;
      const char* dir = std::getenv("BARDOWN_FO_LOG");
      FILE* f = std::fopen((std::string(dir) + "/player_" + std::to_string(int8_t(base[p + 57])) + ".bin").c_str(), "wb");
      if (f) std::fwrite(base + p, 1, 8192, f), std::fclose(f);
    }
  }
}

// puck outcome(mgr, winner, loser, f1, f2)
extern "C" REX_FUNC(sub_833CB660) {
  const uint32_t w = ctx.r4.u32;
  const double f1 = ctx.f1.f64, f2 = ctx.f2.f64;
  __imp__sub_833CB660(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    std::fprintf(o, "P %u f %.4f %.4f dir %.4f %.4f speed %.5f K %.4f %.4f", Tick(base), f1, f2, LF(base, kMgr),
                 LF(base, kMgr + 4), LF(base, kMgr + 8), LF(base, 0x83BFA324), LF(base, 0x83BFA354));
    Who(o, base, w);
    std::fprintf(o, "\n");
  }
}

// gesture match(gesture, state, ctx, frame): faceoff gestures only (classified by their first matcher),
// result per gesture state on change, plus dt
const char* FoName(const uint8_t* b, uint32_t g) {
  const uint32_t n = L32(b, g + 16);
  if (n < 1 || n > 4) return nullptr;
  const uint32_t s0 = L32(b, L32(b, g + 20));
  const uint32_t nm = L32(b, s0 + 8);
  const uint32_t m0 = L32(b, L32(b, s0 + 12));
  const bool stick = L32(b, m0) == 0x820a8f54;
  const float a0 = LF(b, m0 + 24), a1 = LF(b, m0 + 28), mm = LF(b, m0 + 32), mx = LF(b, s0 + 24);
  auto near = [](float x, float y) { return x > y - 0.002f && x < y + 0.002f; };
  if (!stick) return (n == 1 && nm == 2 && int32_t(L32(b, m0 + 20)) != 0 && L32(b, L32(b, s0 + 12) + 4) &&
                      L32(b, L32(b, L32(b, s0 + 12) + 4)) == 0x820a8f54 &&
                      near(LF(b, L32(b, L32(b, s0 + 12) + 4) + 24), 5.498f) &&
                      near(LF(b, L32(b, L32(b, s0 + 12) + 4) + 32), 0.76f))
                         ? "FHPushPuckFwd"
                         : nullptr;
  if (n == 2 && near(a0, 0.785f) && near(mm, 0.82f)) return "BHDrawBack";
  if (n == 1 && nm == 2 && near(a0, 0.785f) && near(a1, 2.356f) && near(mm, 0.76f)) return "BHHold";
  if (n == 2 && near(a0, 0.785f) && near(mm, 0.76f) && near(mx, 100)) return "BHStickLift";
  if (n == 1 && nm == 4 && near(a0, 5.498f)) return "BHTieupFwd";
  if (n == 2 && near(a0, 3.927f) && near(mx, 50)) return "FHDrawBack";
  if (n == 1 && nm == 2 && near(a0, 3.927f) && near(mm, 0.76f)) return "FHHold";
  if (n == 2 && nm == 1 && near(a0, 3.927f) && near(mx, 100)) return "FHStickLift";
  if (n == 1 && nm == 3 && near(a0, 5.498f) && near(mx, 10)) return "FHTieupFwd";
  if (n == 2 && nm == 2 && near(a0, 5.498f) && near(a1, 5.498f) && mm == 0.f) {
    const uint32_t m1 = L32(b, L32(b, s0 + 12) + 4);
    return near(LF(b, m1 + 24), 4.363f) ? "MidDrawBack" : "Shoot";
  }
  return nullptr;
}

extern "C" REX_FUNC(sub_82D25760) {
  const uint32_t g = ctx.r3.u32, gs = ctx.r4.u32, frame = ctx.r6.u32;
  __imp__sub_82D25760(ctx, base);
  FILE* o = Out();
  if (!o) return;
  const uint32_t r = ctx.r3.u32;
  std::lock_guard hold(g_lock);
  static std::map<uint32_t, const char*> names;
  auto nit = names.find(g);
  if (nit == names.end()) nit = names.emplace(g, FoName(base, g)).first;
  {
    static std::map<float, int> dts;
    const float dt = LF(base, frame + 8);
    if (++dts[dt] == 1) std::fprintf(o, "DT %u %g\n", Tick(base), dt);
  }
  if (!nit->second) return;
  static std::map<uint32_t, uint32_t> last;
  auto it = last.find(gs);
  if (it != last.end() && it->second == r) return;
  last[gs] = r;
  std::fprintf(o, "G %u st %u %s gs %08x -> %u step %d stimer %g mtimer %g | char %.4f cam %.4f dt %g mirror %u\n",
               Tick(base), State(base), nit->second, gs, r, int32_t(L32(base, gs + 20)), LF(base, gs + 12),
               LF(base, gs + 16), LF(base, frame), LF(base, frame + 4), LF(base, frame + 8), base[frame + 12]);
}

// human centre faceoff moves: when it runs and in which manager state
extern "C" REX_FUNC(sub_83510C50) {
  const uint32_t p = ctx.r3.u32;
  const uint32_t st = State(base);
  __imp__sub_83510C50(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    static uint32_t last = 99;
    if (st != last) {
      last = st;
      std::fprintf(o, "H %u st %u", Tick(base), st);
      Who(o, base, p);
      std::fprintf(o, "\n");
    }
  }
}

// referee anim query inside the drop check sub_834EBC50 (returns there at 0x834EBC78)
extern "C" REX_FUNC(sub_8333E9F0) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_8333E9F0(ctx, base);
  if (lr == 0x834EBC78) {
    if (FILE* o = Out()) {
      const uint32_t st = State(base);
      if (st >= 3 && st <= 5) {
        std::lock_guard hold(g_lock);
        std::fprintf(o, "W %u st %u v %.4f\n", Tick(base), st, ctx.f1.f64);
      }
    }
  }
}

// analog matcher(matcher, flag*, ctx, device ctx): device ctx mirror byte +12 and angles +0 +4, on change
extern "C" REX_FUNC(sub_82D293C8) {
  const uint32_t dev = ctx.r6.u32;
  if (FILE* o = Out(); o && Ok(dev) && std::getenv("BARDOWN_FO_DEV")) {
    std::lock_guard hold(g_lock);
    static std::map<uint32_t, std::string> last;
    char buf[96];
    std::snprintf(buf, sizeof buf, "%u %.4f %.4f", base[dev + 12], LF(base, dev), LF(base, dev + 4));
    auto& l = last[dev];
    const uint32_t st = State(base);
    if (st >= 2 && st <= 5 && l != buf) {
      l = buf;
      std::fprintf(o, "D %u dev %08x mirror %s\n", Tick(base), dev, buf);
    }
  }
  __imp__sub_82D293C8(ctx, base);
}

// mapping update: only to read the frame dt once
extern "C" REX_FUNC(sub_82D284E8) {
  static bool once = false;
  if (!once) {
    if (FILE* o = Out()) {
      std::lock_guard hold(g_lock);
      std::fprintf(o, "DT %g\n", LF(base, ctx.r6.u32 + 8));
      once = true;
    }
  }
  __imp__sub_82D284E8(ctx, base);
}

// what the game presents at stoppages and when, logged:
//   BARDOWN_PRES_LOG=<dir> scripts/run.sh match
// <dir>/pres.txt, one line per event:
//   <ms> <tick> EV <name> <size> <words...>        a message through the event dispatcher (sub_8274C958)
//   <ms> <tick> WHISTLE <reason>                    the whistle (sub_83324590)
//   <ms> <tick> REQ <trigger> <team> <player> <x> <accepted>   a NIS trigger asked for (sub_83553FE8)
//   <ms> <tick> SHOT <item> <name> <words...>       a cinematic file starts loading (sub_83474CE8)
//   <ms> <tick> SHOTEND <item>                      a shot item is destroyed (sub_834719E8)
//   <ms> <tick> CAM <type>                          a camera is created (sub_83467C28)
//   <ms> <tick> ST <name> <old> <new>               a polled value changed
// ms = host milliseconds since load, tick = the game's tick counter (0x83C75BF8 +168)

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

REX_EXTERN(__imp__sub_8274C958);
REX_EXTERN(__imp__sub_83324590);
REX_EXTERN(__imp__sub_83553FE8);
REX_EXTERN(__imp__sub_83474CE8);
REX_EXTERN(__imp__sub_834719E8);
REX_EXTERN(__imp__sub_83467C28);
REX_EXTERN(__imp__sub_83469FD0);
REX_EXTERN(__imp__sub_8345DEE0);
REX_EXTERN(__imp__sub_8345E0E0);
REX_EXTERN(__imp__sub_8345E010);

namespace {

const auto g_start = std::chrono::steady_clock::now();

uint32_t L32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
bool Ok(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

std::recursive_mutex g_lock;

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_PRES_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/pres.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

std::string Str(const uint8_t* base, uint32_t a) {
  std::string s;
  if (!Ok(a)) return "-";
  for (int i = 0; i < 96; ++i) {
    char c = char(base[a + i]);
    if (!c) break;
    s += (c == ' ' ? '_' : c);
  }
  return s.empty() ? "-" : s;
}

// message type names: the dispatcher keys messages by a djb2 hash of the name; the game keeps
// hash -> name tables at 0x83D61D00 / 0x83D62CA0, count at 0x83D63C40
std::string TypeName(const uint8_t* base, uint32_t hash) {
  static std::unordered_map<uint32_t, std::string> cache;
  if (auto it = cache.find(hash); it != cache.end()) return it->second;
  const uint32_t n = L32(base, 0x83D63C40);
  for (uint32_t i = 0; i < n && i < 1000; ++i)
    if (L32(base, 0x83D61D00 + 4 * i) == hash) return cache[hash] = Str(base, L32(base, 0x83D62CA0 + 4 * i));
  char buf[16];
  std::snprintf(buf, sizeof buf, "#%08x", hash);
  return buf;
}

void Head(FILE* o, const uint8_t* base) {
  const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
  std::fprintf(o, "%ld %u ", ms, L32(base, 0x83C75BF8 + 168));
}

// values watched for changes, each read from guest memory
struct Watch {
  const char* name = nullptr;
  uint32_t (*read)(const uint8_t*) = nullptr;
  uint32_t last = 0xDEADBEEF;
};

uint32_t g_mgr = 0;  // the NIS scripts manager, once seen

Watch g_watch[] = {
    {"sitcom", [](const uint8_t* b) { return L32(b, 0x83BFD050); }},
    {"whistlereason", [](const uint8_t* b) { return L32(b, 0x83BFD050 + 12); }},
    {"fomgr", [](const uint8_t* b) { return L32(b, 0x83C75C40); }},
    {"period", [](const uint8_t* b) { return L32(b, 0x83C75BF8 + 432); }},
    {"periodnum", [](const uint8_t* b) { return L32(b, 0x83C75BF8 + 436); }},
    {"clock", [](const uint8_t* b) { return uint32_t(int16_t(b[0x83C75BF8 + 426] << 8 | b[0x83C75BF8 + 427])); }},
    {"clockstopped", [](const uint8_t* b) { return uint32_t(b[0x83C75BF8 + 201]); }},
    {"camobj", [](const uint8_t* b) { return L32(b, 0x83C7F038); }},
    {"replaymode", [](const uint8_t* b) {
       const uint32_t r = L32(b, 0x83C8350C + 44);
       return Ok(r) ? L32(b, r + 36) : 0xFFFFFFFFu;
     }},
    {"nistrigger", [](const uint8_t* b) { return g_mgr ? L32(b, g_mgr + 1012) : 0xFFFFFFFFu; }},
    {"nis216", [](const uint8_t* b) { return g_mgr ? L32(b, g_mgr + 216) : 0xFFFFFFFFu; }},
    {"nisflags", [](const uint8_t* b) { return g_mgr ? uint32_t(b[g_mgr + 1004]) : 0xFFFFFFFFu; }},
    // the item list the script built (+220, count first) and which one plays (+224)
    {"nisitem", [](const uint8_t* b) { return g_mgr ? L32(b, g_mgr + 224) : 0xFFFFFFFFu; }},
    {"nisitems", [](const uint8_t* b) {
       const uint32_t l = g_mgr ? L32(b, g_mgr + 220) : 0;
       return Ok(l) ? L32(b, l) : 0xFFFFFFFFu;
     }},
};

long g_last_poll = -1;

// the game tick stops during cinematics, so poll on host time (about once a frame)
void Poll(FILE* o, const uint8_t* base) {
  const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count());
  if (ms - g_last_poll < 15) return;
  g_last_poll = ms;
  for (auto& w : g_watch) {
    const uint32_t v = w.read(base);
    if (v == w.last) continue;
    Head(o, base);
    std::fprintf(o, "ST %s %d %d", w.name, int32_t(w.last), int32_t(v));
    // the item now playing: its object and vtable (the item kind)
    if (!std::strcmp(w.name, "nisitem") && g_mgr) {
      const uint32_t l = L32(base, g_mgr + 220);
      if (Ok(l) && v < 64) {
        const uint32_t it = L32(base, l + 4 * (v + 1));
        if (Ok(it)) std::fprintf(o, " item %08x vt %08x", it, L32(base, it));
      }
    }
    std::fprintf(o, "\n");
    w.last = v;
  }
  static uint32_t n = 0;
  if (++n % 120 == 0) std::fflush(o);
}

}  // namespace

// r3 dispatcher, r4 base type slot, r5 type slot (hash at +0), r6 payload, r7 size
extern "C" REX_FUNC(sub_8274C958) {
  if (FILE* o = Out(); o && Ok(ctx.r5.u32)) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    const std::string name = TypeName(base, L32(base, ctx.r5.u32));
    static std::unordered_map<std::string, uint32_t> seen;
    // a few message kinds fire every tick; keep their first ones and then a sample
    static std::string last;
    std::string key = name;
    if (Ok(ctx.r6.u32))
      for (uint32_t i = 0; i < ctx.r7.u32 / 4 && i < 12; ++i) key += ' ' + std::to_string(L32(base, ctx.r6.u32 + 4 * i));
    const bool repeat = key == last;
    last = key;
    const uint32_t k = repeat ? 1 : seen[name]++;
    const bool keep = name.rfind("OverlayEvent", 0) == 0 || name.rfind("GameEvent", 0) == 0;
    if (!repeat && (keep || k < 3000 || k % 200 == 0)) {
      Head(o, base);
      const uint32_t size = ctx.r7.u32, p = ctx.r6.u32;
      std::fprintf(o, "EV %s %u", name.c_str(), size);
      if (Ok(p))
        for (uint32_t i = 0; i < size / 4 && i < 12; ++i) std::fprintf(o, " %x", L32(base, p + 4 * i));
      std::fprintf(o, "\n");
    }
  }
  __imp__sub_8274C958(ctx, base);
}

extern "C" REX_FUNC(sub_83324590) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    Head(o, base);
    std::fprintf(o, "WHISTLE %d\n", int32_t(ctx.r3.u32));
  }
  __imp__sub_83324590(ctx, base);
}

extern "C" REX_FUNC(sub_83553FE8) {
  const int32_t trig = int32_t(ctx.r3.u32), team = int32_t(ctx.r4.u32), player = int32_t(ctx.r5.u32),
                x = int32_t(ctx.r6.u32);
  __imp__sub_83553FE8(ctx, base);
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    Head(o, base);
    std::fprintf(o, "REQ %d %d %d %d %u\n", trig, team, player, x, ctx.r3.u32 & 0xFF);
  }
}

extern "C" REX_FUNC(sub_83474CE8) {
  if (FILE* o = Out(); o && Ok(ctx.r3.u32)) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    Head(o, base);
    const uint32_t it = ctx.r3.u32;
    std::fprintf(o, "SHOT %08x %s", it, Str(base, L32(base, it + 20)).c_str());
    for (int i = 1; i < 12; ++i) std::fprintf(o, " %x", L32(base, it + 4 * i));
    std::fprintf(o, "\n");
  }
  __imp__sub_83474CE8(ctx, base);
}

extern "C" REX_FUNC(sub_834719E8) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    Head(o, base);
    std::fprintf(o, "SHOTEND %08x\n", ctx.r3.u32);
  }
  __imp__sub_834719E8(ctx, base);
}

extern "C" REX_FUNC(sub_83467C28) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    Poll(o, base);
    Head(o, base);
    std::fprintf(o, "CAM %d\n", int32_t(ctx.r3.u32));
  }
  __imp__sub_83467C28(ctx, base);
}

// a switch node (op 102..156) with the value it switches on: r4 the decoded node (+12 its child table)
extern "C" REX_FUNC(sub_83469FD0) {
  if (FILE* o = Out(); o && Ok(ctx.r4.u32)) {
    std::lock_guard hold(g_lock);
    if (Ok(ctx.r3.u32)) g_mgr = ctx.r3.u32;
    Poll(o, base);
    Head(o, base);
    const uint32_t scripts = g_mgr ? L32(base, g_mgr + 4) : 0;
    const uint32_t kids = L32(base, ctx.r4.u32 + 12);
    std::fprintf(o, "SWITCH op %u value %d kids %d\n", uint32_t(base[ctx.r4.u32 + 6] << 8 | base[ctx.r4.u32 + 7]),
                 int32_t(ctx.r5.u32), scripts ? int(kids - scripts) / 2 : -1);
  }
  __imp__sub_83469FD0(ctx, base);
}

// op 15: a transition (r6 kind, r7 frames) or the replay highlights
extern "C" REX_FUNC(sub_8345DEE0) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    if (Ok(ctx.r3.u32)) g_mgr = ctx.r3.u32;
    Head(o, base);
    std::fprintf(o, "TRANS a %d kind %d frames %d\n", int32_t(ctx.r5.u32), int32_t(ctx.r6.u32), int32_t(ctx.r7.u32));
  }
  __imp__sub_8345DEE0(ctx, base);
}

extern "C" REX_FUNC(sub_8345E0E0) {
  if (FILE* o = Out()) {
    std::lock_guard hold(g_lock);
    Head(o, base);
    std::fprintf(o, "REPLAY a %d arg %d\n", int32_t(ctx.r5.u32), int32_t(ctx.r6.u32));
  }
  __imp__sub_8345E0E0(ctx, base);
}

extern "C" REX_FUNC(sub_8345E010) {
  if (FILE* o = Out(); o && Ok(ctx.r4.u32)) {
    std::lock_guard hold(g_lock);
    Head(o, base);
    const uint32_t a = L32(base, ctx.r4.u32 + 8);
    std::fprintf(o, "TAG");
    for (int i = 0; i < 4; ++i) std::fprintf(o, " %u", uint32_t(base[a + 2 * i] << 8 | base[a + 2 * i + 1]));
    std::fprintf(o, "\n");
  }
  __imp__sub_8345E010(ctx, base);
}

// the state of play every tick, for measuring the game's AI:
//   BARDOWN_MATCH_LOG=<dir> scripts/run.sh match
// PM::Update (sub_833636B0) runs once a tick in a match; before it, <dir>/ticks.bin gets
//   "TICK", u64 steady-clock ns, f32 puck x y z, i32 owner (player index, -1 loose), f32 seconds owned,
//   u8 player count, then per player: u32 index byte, u32 team (0, 1, 2 none), u32 kind (0 skater, 1 goalie),
//   u32 action state (the words as they are), u32 object address, f32 x z, f32 vx vz (per tick)
// BARDOWN_ALL_CPU=1 hands every player to the computer each tick (controller byte +556 = 0xFF, a pad's
// own number below 0x80), so the match is CPU against CPU
// host byte order. layout from logs/puck/POSSESSION.md: puck manager *(0x83BFD14C) (+64 position,
// +180 owner, +224 seconds owned), players 0x83BFD1F0[i] (+32 body -> +48 position, +80 velocity;
// +40 kind, +57 index, +60 action state). the team comes from the two team lists, 0x83BFD09C[t]
// -> +88 first player, each player's next at +616 (POSSESSION.md section 3); 2 = on neither

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_833636B0);
REX_EXTERN(__imp__sub_82905438);
REX_EXTERN(__imp__sub_83232300);
REX_EXTERN(__imp__sub_83226FD0);
REX_EXTERN(__imp__sub_832303C0);
REX_EXTERN(__imp__sub_82D53290);

namespace {

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

// guest heap and image pointers: 0x40000000 up to the 0xE0000000 window (the AI heap is at 0xA9...)
bool Plausible(uint32_t p) { return p >= 0x40000000u && p < 0xE0000000u; }

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_MATCH_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/ticks.bin").c_str(), "wb") : nullptr;
  }();
  return out;
}

// ticks of PM::Update so far, to tie anim.bin's records to ticks.bin's
uint32_t g_tick = 0;

FILE* AnimOut() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_MATCH_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/anim.bin").c_str(), "wb") : nullptr;
  }();
  return out;
}

}  // namespace

// a skater's animation, every time the game reads it back (sub_82905438, the PreUpdate rig op: sk+132
// current anim type, +136 subtype, +76 frames in the type, velocity at +1440): <dir>/anim.bin gets
//   "ANIM", u32 tick, u32 sk, i32 type, i32 subtype, i32 frames in type, f32 vx vy vz
// the sk is matched to a player in ticks.bin by velocity
extern "C" REX_FUNC(sub_82905438) {
  const uint32_t sk = ctx.r3.u32;
  __imp__sub_82905438(ctx, base);
  if (FILE* out = AnimOut(); out && Plausible(sk)) {
    // one write a record: it's called from more than one thread
    uint8_t rec[36];
    const uint32_t head[5] = {g_tick, sk, Load32(base, sk + 132), Load32(base, sk + 136), Load32(base, sk + 76)};
    const float v[3] = {LoadF(base, sk + 1440), LoadF(base, sk + 1444), LoadF(base, sk + 1448)};
    std::memcpy(rec, "ANIM", 4);
    std::memcpy(rec + 4, head, 20);
    std::memcpy(rec + 24, v, 12);
    std::fwrite(rec, 1, sizeof rec, out);
  }
}

extern "C" REX_FUNC(sub_833636B0) {
  ++g_tick;
  if (FILE* out = Out()) {
    uint32_t g = Load32(base, 0x83BFD14C);
    if (Plausible(g)) {
      std::vector<uint8_t> rec;
      auto put = [&](const void* p, size_t n) {
        rec.insert(rec.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
      };
      auto f32 = [&](float f) { put(&f, 4); };
      put("TICK", 4);
      auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
      put(&ns, 8);
      f32(LoadF(base, g + 64));
      f32(LoadF(base, g + 68));
      f32(LoadF(base, g + 72));
      auto owner = int32_t(Load32(base, g + 180));
      put(&owner, 4);
      f32(LoadF(base, g + 224));
      std::vector<uint8_t> players;
      uint8_t count = 0;
      uint32_t members[2][40] = {};
      size_t sizes[2] = {};
      for (uint32_t t = 0; t < 2; ++t) {
        uint32_t team = Load32(base, 0x83BFD09C + 4 * t);
        uint32_t m = Plausible(team) ? Load32(base, team + 88) : 0;
        while (Plausible(m) && sizes[t] < 40) {
          members[t][sizes[t]++] = m;
          m = Load32(base, m + 616);
        }
      }
      auto team_of = [&](uint32_t p) -> uint32_t {
        for (uint32_t t = 0; t < 2; ++t)
          for (size_t k = 0; k < sizes[t]; ++k)
            if (members[t][k] == p) return t;
        return 2;
      };
      for (uint32_t i = 0; i < 64; ++i) {
        uint32_t p = Load32(base, 0x83BFD1F0 + 4 * i);
        if (!Plausible(p)) continue;
        uint32_t body = Load32(base, p + 32);
        if (!Plausible(body)) continue;
        static const bool all_cpu = std::getenv("BARDOWN_ALL_CPU") != nullptr;
        if (all_cpu && base[p + 556] < 0x80) base[p + 556] = 0xFF;
        uint32_t head[5] = {base[p + 57], team_of(p), Load32(base, p + 40), Load32(base, p + 60), p};
        float v[4] = {LoadF(base, body + 48), LoadF(base, body + 56), LoadF(base, body + 80), LoadF(base, body + 88)};
        players.insert(players.end(), reinterpret_cast<uint8_t*>(head), reinterpret_cast<uint8_t*>(head) + 20);
        players.insert(players.end(), reinterpret_cast<uint8_t*>(v), reinterpret_cast<uint8_t*>(v) + 16);
        ++count;
      }
      put(&count, 1);
      rec.insert(rec.end(), players.begin(), players.end());
      // once, ten seconds into play: every player object whole (8 KB each) to players.bin, as
      // "PLYR", u32 address, the bytes as the guest has them; for finding who's who and his ratings
      static int ticks = 0;
      if (++ticks == 300)
        if (const char* dir = std::getenv("BARDOWN_MATCH_LOG"))
          if (FILE* dump = std::fopen((std::string(dir) + "/players.bin").c_str(), "wb")) {
            for (uint32_t i = 0; i < 64; ++i) {
              uint32_t p = Load32(base, 0x83BFD1F0 + 4 * i);
              if (!Plausible(p)) continue;
              std::fwrite("PLYR", 1, 4, dump);
              std::fwrite(&p, 4, 1, dump);
              std::fwrite(base + p, 1, 8192, dump);
            }
            std::fclose(dump);
          }
      std::fwrite(rec.data(), 1, rec.size(), out);
      // a second at a time, so a run that's killed keeps what it had
      static int n = 0;
      if (++n % 30 == 0) std::fflush(out);
    }
  }
  __imp__sub_833636B0(ctx, base);
}

// every clip, blend tree, chooser and flow node a character starts (docs: the stick decode's hook): the
// base Controller constructor sub_832303C0 (r3 controller, r4 runtime asset, r5 create params) and the
// SEQ fast path sub_82D53290 (r4 asset, r5 params; skips the constructor for a single-track SEQ). to
// <dir>/clips.bin:
//   "CTRL" or "SEQ ", u32 tick, u32 player (0x83BFD1F0 entry, 0 when not known yet), u32 G, u32 asset
// G (params+0) is the character's game-state block; its AiData.PlayerId names the player. the runtime
// assets have no names: <dir>/assets.bin gets "ASST", u32 asset, the 16 guid bytes as loaded, from the
// loader's registration sub_83232300 (r4 asset, r5 guid), for joining against the stream's guids
namespace {

FILE* LogFile(const char* name) {
  const char* dir = std::getenv("BARDOWN_MATCH_LOG");
  return dir && *dir ? std::fopen((std::string(dir) + "/" + name).c_str(), "wb") : nullptr;
}

FILE* ClipsOut() {
  static FILE* out = LogFile("clips.bin");
  return out;
}

FILE* AssetsOut() {
  static FILE* out = LogFile("assets.bin");
  return out;
}

std::mutex g_layouts_lock;
std::unordered_map<uint32_t, uint32_t> g_player_id_at;  // layout -> offset of AiData.PlayerId in the values
thread_local int t_in_seq = 0;

std::string GuestString(const uint8_t* base, uint32_t addr) {
  if (!Plausible(addr)) return {};
  std::string out;
  for (uint32_t i = 0; i < 64; ++i) {
    char c = char(base[addr + i]);
    if (!c) break;
    out += c;
  }
  return out;
}

uint32_t PlayerOf(const uint8_t* base, uint32_t params) {
  if (!Plausible(params)) return 0;
  const uint32_t g = Load32(base, params), layout = Load32(base, params + 8);
  if (!Plausible(g)) return 0;
  uint32_t off;
  {
    std::lock_guard<std::mutex> hold(g_layouts_lock);
    auto it = g_player_id_at.find(layout);
    if (it == g_player_id_at.end()) return 0;
    off = it->second;
  }
  uint16_t values;
  std::memcpy(&values, base + g + 28, 2);
  values = __builtin_bswap16(values);
  if (!values) return 0;
  const uint32_t id = Load32(base, g + values + off);
  if (id >= 64) return 0;
  return Load32(base, 0x83BFD1F0 + 4 * id);
}

void Started(const uint8_t* base, const char* kind, uint32_t asset, uint32_t params) {
  FILE* out = ClipsOut();
  if (!out) return;
  uint8_t rec[20];
  const uint32_t g = Plausible(params) ? Load32(base, params) : 0;
  const uint32_t v[4] = {g_tick, PlayerOf(base, params), g, asset};
  std::memcpy(rec, kind, 4);
  std::memcpy(rec + 4, v, 16);
  std::fwrite(rec, 1, sizeof rec, out);
  // now and then, so a run that's killed keeps what it had; the assets are all in by the first
  static std::atomic<uint32_t> n{0};
  if (n.fetch_add(1) % 4096 == 0) {
    std::fflush(out);
    if (FILE* assets = AssetsOut()) std::fflush(assets);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_83232300) {
  const uint32_t asset = ctx.r4.u32, guid = ctx.r5.u32;
  if (FILE* out = AssetsOut(); out && Plausible(guid)) {
    uint8_t rec[24];
    std::memcpy(rec, "ASST", 4);
    std::memcpy(rec + 4, &asset, 4);
    std::memcpy(rec + 8, base + guid, 16);
    std::fwrite(rec, 1, sizeof rec, out);
  }
  __imp__sub_83232300(ctx, base);
}

extern "C" REX_FUNC(sub_83226FD0) {
  const uint32_t out = ctx.r3.u32, layout = ctx.r4.u32, group = ctx.r5.u32, name = ctx.r6.u32;
  __imp__sub_83226FD0(ctx, base);
  if (ClipsOut() && Plausible(out) && GuestString(base, group) == "AiData" && GuestString(base, name) == "PlayerId") {
    std::lock_guard<std::mutex> hold(g_layouts_lock);
    g_player_id_at[layout] = Load32(base, out + 8);
  }
}

extern "C" REX_FUNC(sub_832303C0) {
  if (!t_in_seq) Started(base, "CTRL", ctx.r4.u32, ctx.r5.u32);
  __imp__sub_832303C0(ctx, base);
}

extern "C" REX_FUNC(sub_82D53290) {
  Started(base, "SEQ ", ctx.r4.u32, ctx.r5.u32);
  ++t_in_seq;
  __imp__sub_82D53290(ctx, base);
  --t_in_seq;
}

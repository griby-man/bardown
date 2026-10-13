// which commentary lines the game picks:
//   BARDOWN_SPEECH_LOG=<dir> scripts/run.sh match
// <dir>/speech.txt, one block per SentenceGroup::ChooseSentence (sub_82787AA8):
//   C <ns> <module> <seed> <group name> <param count> <param values...>
//   T <sentence id> <ok> <best score after>          one per Sentence::Choose (sub_82787680)
//   R <pool> <sample> <score>                         one per repetition score query (sub_8249B4A8)
//   O <ok> <count> <sample:pool...>                   what the group handed back to be played
//   X <team 0 audio id> <city 0> <team 1 audio id> <city 1>   the two sides, as the announcer reads them
// and every event queued on the context system (sub_8244F468, the trigger_* events the CSIS script
// picks a context from):
//   E <ns> <event name> <param count> <param values...>
// module = the SpeechImplementation's sentence data (pbp and the arena announcer differ), seed = the
// shared rand state at 0x83D6E044 on entry. the param values are what the event carries, in the
// order of its EVT0 parameter list

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/ppc.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_82787AA8);
REX_EXTERN(__imp__sub_82787680);
REX_EXTERN(__imp__sub_8249B4A8);
REX_EXTERN(__imp__sub_8244F468);

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

FILE* Out() {
  static FILE* out = [] {
    const char* dir = std::getenv("BARDOWN_SPEECH_LOG");
    return dir && *dir ? std::fopen((std::string(dir) + "/speech.txt").c_str(), "w") : nullptr;
  }();
  return out;
}

std::recursive_mutex mutex;
thread_local int depth = 0;  // inside a ChooseSentence on this thread

std::string Name(const uint8_t* base, uint32_t p) {
  std::string s;
  if (p < 0x40000000u || p >= 0xE0000000u) return "?";
  for (int i = 0; i < 128; ++i) {
    char c = static_cast<char>(base[p + i]);
    if (!c) break;
    s += c;
  }
  return s;
}

}  // namespace

extern "C" REX_FUNC(sub_82787AA8) {
  FILE* out = Out();
  if (!out) {
    __imp__sub_82787AA8(ctx, base);
    return;
  }
  std::lock_guard lock(mutex);
  const uint32_t module = ctx.r3.u32, event = ctx.r10.u32, result = ctx.r9.u32;
  const uint32_t values = Load32(base, ctx.r1.u32 + 84);  // the caller's stack argument
  const uint32_t count = Load32(base, event + 4);
  long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();
  std::fprintf(out, "C %lld %08X %08X %s %u", ns, module, Load32(base, 0x83D6E044),
               Name(base, Load32(base, event)).c_str(), count);
  for (uint32_t i = 0; i < count && i < 64; ++i) std::fprintf(out, " %d", int32_t(Load32(base, values + 4 * i)));
  std::fprintf(out, "\n");
  // the two sides' team objects (0x83BFD4E8, 38520 bytes each): +8 team audio id, +10 city audio id
  if (uint32_t teams = Load32(base, 0x83BFD4E8); teams >= 0x40000000u && teams < 0xE0000000u) {
    auto u16 = [&](uint32_t a) { return (uint32_t(base[a]) << 8) | base[a + 1]; };
    std::fprintf(out, "X %u %u %u %u\n", u16(teams + 8), u16(teams + 10), u16(teams + 38520 + 8),
                 u16(teams + 38520 + 10));
  }
  ++depth;
  __imp__sub_82787AA8(ctx, base);
  --depth;
  const uint32_t ok = ctx.r3.u32 & 0xFF;
  const uint32_t b = Load32(base, result), e = Load32(base, result + 4);
  const uint32_t n = e >= b ? (e - b) / 12 : 0;
  std::fprintf(out, "O %u %u", ok, n);
  for (uint32_t i = 0; i < n && i < 64; ++i)
    std::fprintf(out, " %u:%u", Load32(base, b + 12 * i), Load32(base, b + 12 * i + 4));
  std::fprintf(out, "\n");
  std::fflush(out);
}

extern "C" REX_FUNC(sub_82787680) {
  FILE* out = depth ? Out() : nullptr;
  const uint32_t sentence = ctx.r3.u32;
  const uint32_t best = Load32(base, ctx.r1.u32 + 92);  // stack argument: the group's best score
  __imp__sub_82787680(ctx, base);
  if (out) {
    std::lock_guard lock(mutex);
    std::fprintf(out, "T %u %u %g\n", Load32(base, sentence), ctx.r3.u32 & 0xFF, LoadF(base, best));
  }
}

extern "C" REX_FUNC(sub_8249B4A8) {
  FILE* out = depth ? Out() : nullptr;
  const uint32_t pool = ctx.r4.u32, sample = ctx.r5.u32;
  __imp__sub_8249B4A8(ctx, base);
  if (out) {
    std::lock_guard lock(mutex);
    std::fprintf(out, "R %u %u %g\n", pool, sample, ctx.f1.f64);
  }
}

extern "C" REX_FUNC(sub_8244F468) {
  if (FILE* out = Out()) {
    std::lock_guard lock(mutex);
    const uint32_t values = ctx.r4.u32, event = ctx.r5.u32;
    const uint32_t count = Load32(base, event + 4);
    long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    std::fprintf(out, "E %lld %s %u", ns, Name(base, Load32(base, event)).c_str(), count);
    for (uint32_t i = 0; i < count && i < 64; ++i) std::fprintf(out, " %d", int32_t(Load32(base, values + 4 * i)));
    std::fprintf(out, "\n");
  }
  __imp__sub_8244F468(ctx, base);
}

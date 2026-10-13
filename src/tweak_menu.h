// the game's own tuning values (its "tweaks") in the settings overlay (F1), under "NHL 14": read live from the
// running game, so nothing of the game ships with bardown. the game links every tweak into one list at
// 0x83D6C490 while it boots; once that list stops growing, each float / int / bool tweak (and each entry of
// the small slider arrays) becomes a setting whose value is the game's memory itself. saved values come back
// on their own: the config loader holds values for settings that don't exist yet and applies them when
// they're registered

#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

namespace bardown {

namespace tweak_menu {

constexpr uint32_t kList = 0x83D6C490;
constexpr uint32_t kFloat = 0x8233B78C, kInt = 0x8233B780, kBool = 0x8233B774, kArray = 0x8233B7C8;
constexpr int kMaxArray = 8;  // the difficulty and game-style sliders; longer arrays are curves

inline uint32_t U32(const uint8_t* m, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, m + a, 4);
  return __builtin_bswap32(v);
}
inline void PutU32(uint8_t* m, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(m + a, &v, 4);
}
inline float F32(const uint8_t* m, uint32_t a) {
  uint32_t u = U32(m, a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
inline void PutF32(uint8_t* m, uint32_t a, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  PutU32(m, a, u);
}
// short enough to read, exact enough that an unchanged value prints as its default
inline std::string FloatText(float f) {
  char b[32];
  std::snprintf(b, sizeof b, "%.9g", double(f));
  return b;
}
// a guest string, bounded
inline std::string Str(const uint8_t* m, uint32_t a) {
  if (a < 0x82000000u || a >= 0x84000000u) return {};
  return std::string(reinterpret_cast<const char*>(m + a), strnlen(reinterpret_cast<const char*>(m + a), 128));
}

// the list's length, and whether every entry looks sane
inline int Count(const uint8_t* m) {
  int n = 0;
  for (uint32_t o = U32(m, kList); o && n < 100000; o = U32(m, o + 20)) {
    if (o < 0x82000000u || o >= 0xC0000000u) return -1;
    ++n;
  }
  return n;
}

inline void Register(uint8_t* m) {
  std::unordered_set<std::string> seen;
  int added = 0;
  for (uint32_t o = U32(m, kList); o; o = U32(m, o + 20)) {
    const uint32_t vt = U32(m, o);
    std::string name = Str(m, U32(m, o + 8)), category = Str(m, U32(m, o + 4));
    if (name.empty()) continue;
    std::string key = "nhl_" + name;
    for (int k = 2; !seen.insert(key).second; ++k) key = "nhl_" + name + "_" + std::to_string(k);
    rex::cvar::FlagEntry e;
    e.category = "NHL 14/" + (category.empty() ? std::string("Other") : category);
    e.description = name + " (the game's own tuning value; changes take effect at once)";
    e.command_callback = [](std::string_view) {};
    if (vt == kFloat) {
      e.type = rex::cvar::FlagType::Double;
      e.getter = [m, o] { return FloatText(F32(m, o + 40)); };
      e.setter = [m, o](std::string_view v) {
        double d = 0;
        if (!rex::cvar::ParseDouble(v, d)) return false;
        PutF32(m, o + 40, float(d));
        return true;
      };
      const float lo = F32(m, o + 32), hi = F32(m, o + 36);
      if (lo < hi) e.constraints.min = lo, e.constraints.max = hi;
    } else if (vt == kInt) {
      e.type = rex::cvar::FlagType::Int32;
      e.getter = [m, o] { return std::to_string(int32_t(U32(m, o + 40))); };
      e.setter = [m, o](std::string_view v) {
        try {
          PutU32(m, o + 40, uint32_t(std::stoi(std::string(v))));
        } catch (...) {
          return false;
        }
        return true;
      };
    } else if (vt == kBool) {
      e.type = rex::cvar::FlagType::Boolean;
      e.getter = [m, o] { return std::string(m[o + 29] ? "true" : "false"); };
      e.setter = [m, o](std::string_view v) {
        m[o + 29] = (v == "true" || v == "1") ? 1 : 0;
        return true;
      };
    } else if (vt == kArray) {
      // each entry its own setting; the game reads the live copy behind +44 when there is one
      const int n = int(U32(m, o + 28));
      if (n < 1 || n > kMaxArray) continue;
      for (int i = 0; i < n; ++i) {
        rex::cvar::FlagEntry a = e;
        a.name = key + "_" + std::to_string(i);
        a.type = rex::cvar::FlagType::Double;
        a.description = name + "[" + std::to_string(i) + "] (an entry of the game's own slider array)";
        a.getter = [m, o, i] { return FloatText(F32(m, U32(m, o + 32) + 4 * i)); };
        a.setter = [m, o, i](std::string_view v) {
          double d = 0;
          if (!rex::cvar::ParseDouble(v, d)) return false;
          PutF32(m, U32(m, o + 32) + 4 * i, float(d));
          if (const uint32_t live = U32(m, o + 44)) PutF32(m, U32(m, live + 32) + 4 * i, float(d));
          return true;
        };
        a.default_value = a.getter();
        if (rex::cvar::RegisterFlag(std::move(a))) ++added;
      }
      continue;
    } else {
      continue;
    }
    e.name = key;
    e.default_value = e.getter();
    if (rex::cvar::RegisterFlag(std::move(e))) ++added;
  }
  REXLOG_INFO("[bardown] tweak menu: {} of the game's tuning values in the settings overlay (F1, NHL 14)", added);
  // BARDOWN_TWEAK_ECHO=<setting>: log that setting's live value once it's registered (checks saved values land)
  if (const char* echo = std::getenv("BARDOWN_TWEAK_ECHO"))
    REXLOG_INFO("[bardown] tweak menu: {} = {}", echo, rex::cvar::GetFlagByName(echo));
}

}  // namespace tweak_menu

// waits for the game to finish linking its tweaks (the list stops growing), then registers them
inline void StartTweakMenu(rex::Runtime* runtime) {
  auto* m = reinterpret_cast<uint8_t*>(runtime->memory()->TranslateVirtual(0));
  std::thread([m] {
    int last = -1, steady = 0;
    for (int tries = 0; tries < 600; ++tries) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      const int n = tweak_menu::Count(m);
      if (n > 1000 && n == last) {
        if (++steady >= 4) {
          tweak_menu::Register(m);
          return;
        }
      } else {
        steady = 0;
      }
      last = n;
    }
    REXLOG_WARN("[bardown] tweak menu: the game's tweak list never settled; the NHL 14 settings are off");
  }).detach();
}

}  // namespace bardown

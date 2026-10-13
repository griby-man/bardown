// which recompiled functions read a range of guest memory. debug builds only:
//   cmake -B out/build/linux-amd64-watch ... -DCMAKE_CXX_FLAGS="... -DBARDOWN_WATCH_LOADS"
// the pch's REX_LOAD_* call bardown_watch_hit for addresses in [g_bw_lo, g_bw_lo + g_bw_len).
//
// BARDOWN_WATCH_GD=<type hash hex> BARDOWN_WATCH_OUT=<dir>: a thread finds every GD object of that
// type in guest memory (chunk magic "GD.DATAl", type at +0x20, either byte order), writes their
// addresses to <dir>/objects.txt, watches the span they cover and logs each new (function, address)
// pair to <dir>/hits.txt

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

extern "C" volatile uint32_t g_bw_lo, g_bw_len;

namespace bardown {

struct LoadWatch {
  std::mutex mutex;
  std::unordered_set<uint64_t> seen;
  std::ofstream hits;
  std::vector<std::pair<uint32_t, uint32_t>> objects;  // start, size; sorted
};

inline LoadWatch& Watch() {
  static LoadWatch w;
  return w;
}

}  // namespace bardown


namespace bardown {

// readable host ranges inside the guest's 4 GB window
inline std::vector<std::pair<uint32_t, uint32_t>> ReadableGuestRanges(uintptr_t membase) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  std::ifstream maps("/proc/self/maps");
  for (std::string line; std::getline(maps, line);) {
    unsigned long long lo64 = 0, hi64 = 0;
    char perms[5] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %4s", &lo64, &hi64, perms) != 3 || perms[0] != 'r') continue;
    uintptr_t lo = uintptr_t(lo64), hi = uintptr_t(hi64);
    uintptr_t end = membase + 0x100000000ull;
    if (hi <= membase || lo >= end) continue;
    lo = std::max(lo, membase);
    hi = std::min(hi, end);
    out.push_back({uint32_t(lo - membase), uint32_t(hi - lo)});
  }
  return out;
}

inline void StartLoadWatch([[maybe_unused]] rex::Runtime* runtime) {
#ifdef BARDOWN_WATCH_LOADS
  const char* type_env = std::getenv("BARDOWN_WATCH_GD");
  const char* dir = std::getenv("BARDOWN_WATCH_OUT");
  // BARDOWN_WATCH_PTR=<ptr>:<off>:<len> (hex): watch [*(ptr) + off, +len), re-read every 2 s
  if (const char* pw = std::getenv("BARDOWN_WATCH_PTR"); pw && dir) {
    uint32_t ptr = 0, off = 0, len = 0;
    if (std::sscanf(pw, "%x:%x:%x", &ptr, &off, &len) != 3) return;
    const auto* mb = reinterpret_cast<const uint8_t*>(runtime->memory()->TranslateVirtual(0));
    Watch().hits.open(std::string(dir) + "/hits.txt");
    std::thread([=] {
      uint32_t last = 0;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        uint32_t v;
        std::memcpy(&v, mb + ptr, 4);
        v = __builtin_bswap32(v);
        if (!v || v == last) continue;
        last = v;
        g_bw_len = 0;
        {
          std::lock_guard lock(Watch().mutex);
          Watch().objects = {{v + off, len}};
        }
        g_bw_lo = v + off;
        g_bw_len = len;
        REXLOG_INFO("[bardown] watch ptr: {:08X}+{:X}", v + off, len);
      }
    }).detach();
    return;
  }
  if (!dir || (!type_env && !std::getenv("BARDOWN_WATCH_SIGS"))) return;
  uint32_t type = type_env ? uint32_t(std::strtoul(type_env, nullptr, 16)) : 0;
  auto* membase = reinterpret_cast<uint8_t*>(runtime->memory()->TranslateVirtual(0));
  Watch().hits.open(std::string(dir) + "/hits.txt");
  std::thread([=] {
    uint8_t le[4], be[4];
    std::memcpy(le, &type, 4);
    for (int i = 0; i < 4; ++i) be[i] = le[3 - i];
    // BARDOWN_WATCH_SIGS=<file>: lines of "<32 bytes hex> <length>"; watch every place one appears
    struct Sig {
      uint8_t bytes[32];
      uint32_t length;
    };
    std::vector<Sig> sigs;
    std::unordered_map<uint64_t, std::vector<size_t>> by_prefix;
    if (const char* sig_file = std::getenv("BARDOWN_WATCH_SIGS")) {
      std::ifstream in(sig_file);
      std::string hex;
      uint32_t length;
      while (in >> hex >> length) {
        if (hex.size() != 64) continue;
        Sig sig{{}, length};
        for (size_t i = 0; i < 32; ++i) sig.bytes[i] = uint8_t(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        uint64_t prefix;
        std::memcpy(&prefix, sig.bytes, 8);
        by_prefix[prefix].push_back(sigs.size());
        sigs.push_back(sig);
      }
      REXLOG_INFO("[bardown] watch: {} signatures", sigs.size());
    }
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      std::vector<std::pair<uint32_t, uint32_t>> found;
      for (auto [lo, len] : ReadableGuestRanges(uintptr_t(membase))) {
        const uint8_t* p = membase + lo;
        for (uint32_t off = 0; off + 0x30 <= len; off += (sigs.empty() ? 16 : 4)) {
          if (!sigs.empty()) {
            uint64_t prefix;
            std::memcpy(&prefix, p + off, 8);
            auto it = by_prefix.find(prefix);
            if (it == by_prefix.end()) continue;
            const auto hit = std::find_if(it->second.begin(), it->second.end(),
                                          [&](size_t i) { return std::memcmp(p + off, sigs[i].bytes, 32) == 0; });
            if (hit != it->second.end()) found.push_back({lo + off, sigs[*hit].length});
            continue;
          }
          if (std::memcmp(p + off, "GD.DATAl", 8) != 0) continue;
          bool little = std::memcmp(p + off + 0x20, le, 4) == 0;
          if (!little && std::memcmp(p + off + 0x20, be, 4) != 0) continue;
          uint32_t size;
          std::memcpy(&size, p + off + 8, 4);
          if (!little) size = __builtin_bswap32(size);
          if (size < 0x30 || size > (16u << 20)) continue;
          found.push_back({lo + off, size});
        }
      }
      if (found.empty()) continue;
      std::sort(found.begin(), found.end());
      std::ofstream objects(std::string(dir) + "/objects.txt");
      for (auto [a, size] : found) objects << std::hex << a << ' ' << size << '\n';
      uint32_t lo = found.front().first, hi = 0;
      for (auto [a, size] : found) hi = std::max(hi, a + size);
      g_bw_len = 0;
      {
        std::lock_guard lock(Watch().mutex);
        Watch().objects = found;
      }
      g_bw_lo = lo;
      g_bw_len = hi - lo;
      REXLOG_INFO("[bardown] watch: {} objects, {:08X}+{:X}", found.size(), lo, uint32_t(g_bw_len));
    }
  }).detach();
#endif
}

}  // namespace bardown

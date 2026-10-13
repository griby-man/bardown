#include "load_watch.h"

extern "C" {
volatile uint32_t g_bw_lo = 0, g_bw_len = 0;

void bardown_watch_hit(const char* func, uint32_t addr) {
  auto& w = bardown::Watch();
  std::lock_guard lock(w.mutex);
  // only inside an object; keyed by offset so every clip counts once per field
  auto it = std::upper_bound(w.objects.begin(), w.objects.end(), std::make_pair(addr, ~0u));
  if (it == w.objects.begin()) return;
  --it;
  uint32_t off = addr - it->first;
  if (off >= it->second) return;
  uint64_t key = (uint64_t(reinterpret_cast<uintptr_t>(func)) << 32) ^ off;
  if (w.seen.size() > 2000000 || !w.seen.insert(key).second || !w.hits) return;
  w.hits << func << ' ' << std::hex << it->first << ' ' << off << '\n';
}
}

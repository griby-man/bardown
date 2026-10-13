// samples guest memory on a clock, for measuring what the game does over time (a player's motion
// from the bone palette, say):
//   BARDOWN_SAMPLE="<phys address hex>:<bytes>:<stride hex>:<count>[,...]" BARDOWN_SAMPLE_HZ=60
//   BARDOWN_SAMPLE_OUT=<file>
// each spec reads <bytes> at address, address + stride, ... count times. each sample in the file:
// "SMPL", u64 nanoseconds since start, then every spec's bytes in order, as the guest has them
// (big-endian). nothing synchronises with the game: a sample can catch a buffer mid-write
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>

namespace bardown {

inline void StartMemSampler(rex::Runtime* runtime) {
  const char* spec = std::getenv("BARDOWN_SAMPLE");
  const char* out = std::getenv("BARDOWN_SAMPLE_OUT");
  if (!spec || !out) return;
  struct Range {
    uint32_t address, bytes, stride, count;
  };
  std::vector<Range> ranges;
  std::string all = spec;
  for (size_t at = 0; at < all.size();) {
    size_t comma = all.find(',', at), end = comma == std::string::npos ? all.size() : comma;
    std::string one = all.substr(at, end - at);
    Range r{};
    if (std::sscanf(one.c_str(), "%x:%u:%x:%u", &r.address, &r.bytes, &r.stride, &r.count) == 4 && r.bytes && r.count)
      ranges.push_back(r);
    else
      REXLOG_WARN("[bardown] sample: can't read '{}'", one);
    at = end + 1;
  }
  if (ranges.empty()) return;
  double hz = std::getenv("BARDOWN_SAMPLE_HZ") ? std::atof(std::getenv("BARDOWN_SAMPLE_HZ")) : 60;
  if (!(hz > 0)) hz = 60;
  auto* memory = runtime->memory();
  std::string path = out;
  REXLOG_INFO("[bardown] sample: {} ranges at {} Hz into {}", ranges.size(), hz, path);
  std::thread([=] {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    auto start = std::chrono::steady_clock::now();
    auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1 / hz));
    std::vector<uint8_t> buf;
    for (auto next = start;; next += period) {
      std::this_thread::sleep_until(next);
      uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
      buf.clear();
      for (const auto& r : ranges)
        for (uint32_t i = 0; i < r.count; ++i) {
          const auto* p = static_cast<const uint8_t*>(memory->TranslatePhysical(r.address + i * r.stride));
          buf.insert(buf.end(), p, p + r.bytes);
        }
      std::fwrite("SMPL", 1, 4, f);
      std::fwrite(&ns, 8, 1, f);
      std::fwrite(buf.data(), 1, buf.size(), f);
      std::fflush(f);
    }
  }).detach();
}

}  // namespace bardown

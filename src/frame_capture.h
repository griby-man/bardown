// timed captures of the game's own output frame, so test runs work headless (grim can't see them):
//   BARDOWN_SHOT_DIR=logs/shots BARDOWN_SHOT_EVERY=5 scripts/run.sh ...
// writes <dir>/<seconds>.ppm, RGB

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>
#include <rex/ui/windowed_app_context.h>

namespace bardown {

inline bool WritePpm(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f << "P6\n" << image.width << " " << image.height << "\n255\n";
  std::string row(size_t(image.width) * 3, '\0');
  for (uint32_t y = 0; y < image.height; ++y) {
    const uint8_t* src = image.data.data() + y * image.stride;
    for (uint32_t x = 0; x < image.width; ++x) {
      row[x * 3 + 0] = char(src[x * 4 + 0]);
      row[x * 3 + 1] = char(src[x * 4 + 1]);
      row[x * 3 + 2] = char(src[x * 4 + 2]);
    }
    f.write(row.data(), std::streamsize(row.size()));
  }
  return bool(f);
}

inline void StartFrameCapture(rex::Runtime* runtime, rex::ui::WindowedAppContext& ctx) {
  const char* dir = std::getenv("BARDOWN_SHOT_DIR");
  if (!dir || !*dir) return;
  const char* every_env = std::getenv("BARDOWN_SHOT_EVERY");
  int every = every_env ? std::max(1, std::atoi(every_env)) : 5;
  std::filesystem::path out(dir);
  std::filesystem::create_directories(out);
  std::thread([runtime, &ctx, out, every] {
    for (int t = every;; t += every) {
      std::this_thread::sleep_for(std::chrono::seconds(every));
      // presenter work belongs on the ui thread
      ctx.CallInUIThread([runtime, out, t] {
        auto* gfx = runtime->graphics_system();
        auto* presenter = gfx ? gfx->presenter() : nullptr;
        rex::ui::RawImage image;
        if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return;
        WritePpm(out / fmt::format("{:03}.ppm", t), image);
      });
    }
  }).detach();
  REXLOG_INFO("[bardown] capturing a frame every {}s to {}", every, out.string());
  // BARDOWN_SHOT_STOPPAGE=<dir>: every ~33 ms while the sitcom isn't GAME_ON (plus 3 s after), named
  // <steady clock ms>_<game tick>.ppm, for the broadcast overlays' in/out timing
  if (const char* sd = std::getenv("BARDOWN_SHOT_STOPPAGE"); sd && *sd) {
    std::filesystem::path sdir(sd);
    std::filesystem::create_directories(sdir);
    std::thread([runtime, &ctx, sdir] {
      const auto t0 = std::chrono::steady_clock::now();
      auto* mem = reinterpret_cast<const uint8_t*>(runtime->memory()->TranslateVirtual(0));
      long last_stop = -100000;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
        uint32_t s;
        std::memcpy(&s, mem + 0x83BFD050, 4);
        s = __builtin_bswap32(s);
        uint32_t tick;
        std::memcpy(&tick, mem + 0x83C75BF8 + 168, 4);
        if (s != 4 && s < 12 && __builtin_bswap32(tick) > 0) last_stop = ms;
        if (ms - last_stop > 3000) continue;
        const long abs_ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()).count());
        const uint32_t tk = __builtin_bswap32(tick);
        ctx.CallInUIThread([runtime, sdir, abs_ms, tk] {
          auto* gfx = runtime->graphics_system();
          auto* presenter = gfx ? gfx->presenter() : nullptr;
          rex::ui::RawImage image;
          if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return;
          WritePpm(sdir / fmt::format("{}_{}.ppm", abs_ms, tk), image);
        });
      }
    }).detach();
  }
  // BARDOWN_SHOT_BURST=<second>:<count>: from that second, count frames back to back (burst_000.ppm ...),
  // for looking at motion
  if (const char* burst = std::getenv("BARDOWN_SHOT_BURST")) {
    int at = 0, count = 0;
    if (std::sscanf(burst, "%d:%d", &at, &count) == 2 && count > 0)
      std::thread([runtime, &ctx, out, at, count] {
        std::this_thread::sleep_for(std::chrono::seconds(at));
        for (int k = 0; k < count; ++k) {
          ctx.CallInUIThread([runtime, out, k] {
            auto* gfx = runtime->graphics_system();
            auto* presenter = gfx ? gfx->presenter() : nullptr;
            rex::ui::RawImage image;
            if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width) return;
            WritePpm(out / fmt::format("burst_{:03}.ppm", k), image);
          });
          std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
      }).detach();
  }
}

}  // namespace bardown

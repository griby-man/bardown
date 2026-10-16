// timed renderdoc capture for unattended runs:
//   BARDOWN_RDOC_AT=100 renderdoccmd capture -c out/cap scripts/... bardown ...
// only does anything when renderdoc is already injected into the process

#pragma once

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(__linux__)
#include <dlfcn.h>
#include <renderdoc_app.h>
#endif

#include <rex/logging.h>

namespace bardown {

#if defined(__linux__)
// the injected renderdoc's api, or null when it isn't loaded
inline RENDERDOC_API_1_0_0* RenderDocApi() {
  static RENDERDOC_API_1_0_0* api = [] {
    RENDERDOC_API_1_0_0* found = nullptr;
    void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    auto get_api = mod ? reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI")) : nullptr;
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&found))) found = nullptr;
    return found;
  }();
  return api;
}
#endif

// three captures a few frames apart, now (the F11 bind when renderdoc is loaded)
inline void CaptureRenderDocNow() {
#if defined(__linux__)
  RENDERDOC_API_1_0_0* api = RenderDocApi();
  if (!api) return;
  std::thread([api] {
    for (int i = 0; i < 3; ++i) {
      api->StartFrameCapture(nullptr, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      REXLOG_INFO("[rdoc] capture {} {}", i, api->EndFrameCapture(nullptr, nullptr) ? "written" : "failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
  }).detach();
#endif
}

inline void StartRenderDocTrigger() {
#if defined(__linux__)
  const char* at = std::getenv("BARDOWN_RDOC_AT");
  if (!at || !*at) return;
  if (!RenderDocApi()) {
    REXLOG_WARN("[rdoc] BARDOWN_RDOC_AT set but renderdoc is not loaded");
    return;
  }
  // the same captures as F11, after the given seconds (explicit start/end with wildcard device+window:
  // TriggerCapture keys off the active window, which renderdoc doesn't track under wayland)
  std::thread([first = std::stod(at)] {
    std::this_thread::sleep_for(std::chrono::duration<double>(first));
    CaptureRenderDocNow();
  }).detach();
#endif
}

}  // namespace bardown

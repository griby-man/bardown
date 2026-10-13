// timed renderdoc capture for unattended runs:
//   BARDOWN_RDOC_AT=100 renderdoccmd capture -c out/cap scripts/... bardown ...
// only does anything when renderdoc is already injected into the process

#pragma once

#include <cstdlib>
#include <string>
#include <thread>

#if defined(__linux__)
#include <dlfcn.h>
#include <renderdoc_app.h>
#endif

#include <rex/logging.h>

namespace bardown {

inline void StartRenderDocTrigger() {
#if defined(__linux__)
  const char* at = std::getenv("BARDOWN_RDOC_AT");
  if (!at || !*at) return;
  void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
  if (!mod) {
    REXLOG_WARN("[rdoc] BARDOWN_RDOC_AT set but renderdoc is not loaded");
    return;
  }
  auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
  RENDERDOC_API_1_0_0* api = nullptr;
  if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api)) || !api) {
    REXLOG_WARN("[rdoc] RENDERDOC_GetAPI failed");
    return;
  }
  // several captures a second apart, in case one lands on a loading or menu frame
  double first = std::stod(at);
  std::thread([api, first] {
    std::this_thread::sleep_for(std::chrono::duration<double>(first));
    // explicit start/end with wildcard device+window: TriggerCapture keys off the active
    // window, which renderdoc doesn't track under wayland
    for (int i = 0; i < 3; ++i) {
      api->StartFrameCapture(nullptr, nullptr);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      bool ok = api->EndFrameCapture(nullptr, nullptr);
      REXLOG_INFO("[rdoc] capture {} {}", i, ok ? "written" : "failed");
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }).detach();
#endif
}

}  // namespace bardown

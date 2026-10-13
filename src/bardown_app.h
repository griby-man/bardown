// bardown - NHL 14 (Xbox 360) on the ReXGlue runtime

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include <filesystem>
#include <string>
#include <vector>
#include <ctime>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "first_run.h"

namespace bardown::native {
void Start(rex::Runtime* runtime, const std::filesystem::path& folder, const std::filesystem::path& game);
}

extern const unsigned char kOverlayFont[];
extern const std::size_t kOverlayFontSize;
#include "frame_capture.h"
#include "load_watch.h"
#include "mem_sampler.h"
#include "rdoc_trigger.h"
#include "script_input.h"
#include "tweak_menu.h"

class BardownApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<BardownApp>(new BardownApp(ctx, "bardown",
        PPCImageConfig));
  }

  // BARDOWN_DUMP_IMAGE=<file> writes the loaded xex image (82000000-83E80000) to disk
  // so tools/scan_code_refs.py can look for code pointers the recompiler never saw
  void OnPostLoadXexImage() override {
    const char* out = std::getenv("BARDOWN_DUMP_IMAGE");
    if (!out || !*out) return;
    constexpr uint32_t kBase = 0x82000000, kEnd = 0x83E80000;
    auto* host = runtime()->memory()->TranslateVirtual(kBase);
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(host), kEnd - kBase);
    REXLOG_INFO("[bardown] dumped image {:08X}-{:08X} to {}", kBase, kEnd, out);
  }

  // settings and the setup's choices live with the player's data, not next to the program (which may be
  // read-only once packaged)
  // the one folder a packaged build keeps everything in: settings, saves, the shader cache, logs, and the
  // game extracted from the disc image. an AppImage: the folder it's in. on Windows: the program's own folder
  // when it can be written to (unpacked anywhere), else a folder of the player's. on macOS: the player's
  // Application Support (an app bundle is read-only). a Linux build run from the tree has none
  static std::filesystem::path PackagedHome() {
    if (const char* image = std::getenv("APPIMAGE"); image && *image) return std::filesystem::path(image).parent_path();
#if defined(_WIN32)
    const auto exe = rex::filesystem::GetExecutableFolder();
    std::error_code ec;
    const auto probe = exe / ".bardown-write-test";
    if (std::ofstream(probe).good()) {
      std::filesystem::remove(probe, ec);
      return exe;
    }
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local) return std::filesystem::path(local) / "bardown";
    return exe;
#elif defined(__APPLE__)
    if (const char* user = std::getenv("HOME"); user && *user)
      return std::filesystem::path(user) / "Library" / "Application Support" / "bardown";
    return {};
#else
    return {};
#endif
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (const auto home = PackagedHome(); !home.empty()) {
      home_ = home;
      std::error_code ec;
      std::filesystem::create_directories(home_, ec);
      if (rex::cvar::GetFlagByName("user_data_root").empty()) paths.user_data_root = home_ / "userdata";
      if (rex::cvar::GetFlagByName("cache_root").empty()) paths.cache_root = home_ / "cache";
      paths.config_path = home_ / "bardown.toml";
      // the runtime's default log folder is beside the program, read-only inside the AppImage (creating it
      // threw and killed the launch); one log per start, by date, in the folder instead
      if (rex::cvar::GetFlagByName("log_file").empty()) {
        std::filesystem::create_directories(home_ / "logs", ec);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        rex::cvar::SetFlagByName("log_file", (home_ / "logs" / (std::string("bardown-") + stamp + ".log")).string());
      }
    } else if (!paths.user_data_root.empty()) {
      paths.config_path = paths.user_data_root / "bardown.toml";
    }
    Defaults(paths.config_path);
  }

  // what bardown needs that isn't the runtime's own default, so it starts right with no options at all:
  // - resolution_scale 2: twice the Xbox's 1280x720, much sharper and still 60 fps (3x drops to the 40s)
  // - the fsi render-target path (the other leaves the 3D scene black)
  // - readback on auto (equipment is composited into a render target and read back; without it pads,
  //   gloves and helmets come out black)
  // - the keyboard driving the pad (Enter start, Space A, Backspace B, WASD the stick)
  // - no GPU emulation (the native plugin) while the native renderer is on; the emulator only without it
  // the settings file and the command line still win. the graphics plugin registers its settings only
  // once it's loaded, so those defaults go in the environment it reads then
  // the command line is parsed by now (a plugin's settings given there outrank the environment anyway); the
  // settings file isn't loaded yet, so it's read here
  static bool InConfig(const std::filesystem::path& config, const char* key, bool* value = nullptr) {
    std::error_code ec;
    if (!std::filesystem::exists(config, ec)) return false;
    try {
      const auto table = toml::parse_file(config.string());
      if (!table.contains(key)) return false;
      if (value) *value = table[key].value_or(true);
      return true;
    } catch (const toml::parse_error&) {
      return false;
    }
  }
  static bool SetByPlayer(const std::filesystem::path& config, const char* key) {
    return rex::cvar::GetFlagSource(key) == rex::cvar::Source::kCommandLine || InConfig(config, key);
  }
  // the native renderer is on unless the command line or the settings file turns it off
  static bool NativeRenderer(const std::filesystem::path& config) {
    if (rex::cvar::GetFlagSource("native_renderer") == rex::cvar::Source::kCommandLine)
      return rex::cvar::Query<bool>("native_renderer");
    bool on = true;
    InConfig(config, "native_renderer", &on);
    return on;
  }
  void Defaults(const std::filesystem::path& config) {
    struct Default {
      const char* key;
      const char* value;
      bool late;  // registered by the graphics plugin
    };
    const Default kDefaults[] = {
        {"gpu_plugin", NativeRenderer(config) ? "native" : "xenos", false},
        {"mnk_mode", "true", false},
        {"render_target_path_vulkan", "fsi", true},
        {"readback_resolve", "auto", true},
        {"vulkan_readback_resolve", "true", true},
        {"resolution_scale", "2", true},
    };
    for (const auto& d : kDefaults) {
      if (SetByPlayer(config, d.key)) continue;
      if (d.key == std::string_view("resolution_scale") &&
          (SetByPlayer(config, "draw_resolution_scale_x") || SetByPlayer(config, "draw_resolution_scale_y")))
        continue;
      if (d.late) {
        std::string env = "REX_";
        for (const char* c = d.key; *c; ++c) env += char(std::toupper(uint8_t(*c)));
        if (!std::getenv(env.c_str())) {
#ifdef _WIN32
          _putenv_s(env.c_str(), d.value);
#else
          setenv(env.c_str(), d.value, 0);
#endif
        }
      } else if (const auto* info = rex::cvar::GetFlagInfo(d.key); info && info->source == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(d.key, d.value);
      }
    }
  }
  std::filesystem::path home_;  // PackagedHome(), when there is one

  // no game folder yet, or the saved one is gone or the wrong version: the setup window asks for it
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    rex::PathConfig paths = defaults;
    if (paths.game_data_root.empty()) paths.game_data_root = rex::cvar::GetFlagByName("game_data_root");
    // saved relative to the settings file's folder (SaveGameFolder)
    if (!paths.game_data_root.empty() && paths.game_data_root.is_relative())
      paths.game_data_root = paths.config_path.parent_path() / paths.game_data_root;
    std::string problem;
    if (!paths.game_data_root.empty()) {
      problem = bardown::first_run::CheckGameFolder(paths.game_data_root);
      if (problem.empty()) return paths;
      problem = "The game folder set before (" + paths.game_data_root.string() + ") won't do: " + problem;
    }
    // first run, or the saved folder is gone: the game or its disc image beside the program
    std::filesystem::path found;
    if (!home_.empty()) {
      const auto beside = bardown::first_run::FindBeside(home_);
      if (!beside.folder.empty() && bardown::first_run::CheckGameFolder(beside.folder).empty()) {
        bardown::first_run::SaveGameFolder(paths.config_path, beside.folder);
        paths.game_data_root = beside.folder;
        return paths;
      }
      found = beside.image;
    }
    const auto config = paths.config_path;
    const auto data_dir = home_.empty() ? paths.user_data_root : home_;
    setup_ = std::make_unique<bardown::FirstRunDialog>(
        imgui_drawer(), data_dir, problem, [paths, config, resume](std::filesystem::path dir) mutable {
          bardown::first_run::SaveGameFolder(config, dir);
          paths.game_data_root = std::move(dir);
          resume(paths);
        },
        found);
    return std::nullopt;
  }
  std::unique_ptr<bardown::FirstRunDialog> setup_;

  // the overlays at a size that reads on the screen: the runtime's 10 px bitmap font is a speck at 4K. Roboto,
  // 16 px at 1080 lines and scaled with the screen, and every margin scaled to match
  float OverlayScale() const {
    const uint32_t h = window() ? window()->GetActualPhysicalHeight() : 0;
    return h ? std::clamp(float(h) / 1080.0f, 1.0f, 4.0f) : 2.0f;
  }
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;  // it's the program's own bytes
    cfg.OversampleH = cfg.OversampleV = 2;
    ImFont* font = atlas->AddFontFromMemoryTTF(const_cast<unsigned char*>(kOverlayFont), int(kOverlayFontSize),
                                               16.0f * OverlayScale(), &cfg);
    if (font) ImGui::GetIO().FontDefault = font;
  }
  void OnConfigureStyle(ImGuiStyle& style, rex::ui::Style& ui_style) override {
    rex::ReXApp::OnConfigureStyle(style, ui_style);
    style.ScaleAllSizes(OverlayScale());
  }

  // F1 opens the settings, as recompilations usually have it (the runtime's own default is F4); a key the
  // player picked in the settings stays
  void OnCreateDialogs(rex::ui::ImGuiDrawer*) override {
    const auto* bind = rex::cvar::GetFlagInfo("bind_settings");
    if (bind && bind->source == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName("bind_settings", "F1");
  }

  // the engine probes cache:\<path> before falling back to the .big archives and writes
  // its own scratch there, so it needs a real writable device behind cache:
  void OnPostSetup() override {
    bardown::StartRenderDocTrigger();
    bardown::StartFrameCapture(runtime(), app_context());
    bardown::StartLoadWatch(runtime());
    bardown::StartMemSampler(runtime());
    bardown::StartTweakMenu(runtime());
    bardown::native::Start(runtime(), home_.empty() ? user_data_root() : home_, game_data_root());
    namespace fs = rex::filesystem;
    static constexpr const char* kMountPath = R"(\CACHE)";  // raw: "\C" is not an escape

    auto dir = cache_root() / "guest";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      REXLOG_ERROR("[bardown] cache: mkdir {} failed: {}", dir.string(), ec.message());
      return;
    }
    auto device = std::make_unique<fs::HostPathDevice>(kMountPath, dir, /*read_only=*/false);
    if (!device->Initialize() || !runtime()->file_system()->RegisterDevice(std::move(device))) {
      REXLOG_ERROR("[bardown] cache: mount failed for {}", dir.string());
      return;
    }
    runtime()->file_system()->RegisterSymbolicLink("cache:", kMountPath);
    REXLOG_INFO("[bardown] cache: -> {}", dir.string());

    const char* script = std::getenv("BARDOWN_INPUT");
    if ((script && *script) || std::getenv("BARDOWN_INPUT_FILE")) {
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      input->AddDriver(std::make_unique<bardown::ScriptInputDriver>(script ? script : ""));
    }
  }
};

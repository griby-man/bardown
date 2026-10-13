// first run: bardown needs the player's own NHL 14 (Xbox 360, base disc, no title update). when it has no
// game folder yet, or the saved one is gone, a setup window asks for the extracted game folder or the disc
// image (.iso). an image is extracted once into bardown's own data folder. the game's default.xex must be the
// exact one the recompiled code was built from, or it would crash in strange ways, so it's checked first

#pragma once

#include <SDL3/SDL_dialog.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

namespace bardown {

namespace first_run {

// the default.xex the code was recompiled from (NHL 14, base disc): its size and a 64-bit FNV-1a over it
constexpr uint64_t kXexSize = 10436608;
constexpr uint64_t kXexHash = 0x7ec1dd16272cdd6aull;

inline uint64_t Fnv(std::istream& in) {
  uint64_t h = 0xcbf29ce484222325ull;
  std::vector<char> buf(1 << 20);
  while (in.read(buf.data(), std::streamsize(buf.size())) || in.gcount() > 0) {
    for (std::streamsize i = 0; i < in.gcount(); ++i) h = (h ^ uint8_t(buf[size_t(i)])) * 0x100000001b3ull;
    if (!in) break;
  }
  return h;
}

// empty when the folder holds the right game, else what's wrong, in words for the player
inline std::string CheckGameFolder(const std::filesystem::path& dir) {
  const auto xex = dir / "default.xex";
  std::error_code ec;
  if (!std::filesystem::is_regular_file(xex, ec))
    return "That folder has no default.xex. Pick the folder the game was extracted to (the one with default.xex "
           "in it), or the disc image itself.";
  if (std::filesystem::file_size(xex, ec) != kXexSize)
    return "That's not the NHL 14 bardown was built for. It needs the Xbox 360 base disc, without a title update.";
  if (kXexHash) {
    std::ifstream in(xex, std::ios::binary);
    if (Fnv(in) != kXexHash)
      return "That's not the NHL 14 bardown was built for. It needs the Xbox 360 base disc, without a title update.";
  }
  return {};
}

// copies every file out of a disc image into dest; progress 0..1, false on failure with why
inline bool ExtractImage(const std::filesystem::path& iso, const std::filesystem::path& dest,
                         std::atomic<float>& progress, std::string& why) {
  rex::filesystem::DiscImageDevice disc("\\ISO", iso);
  if (!disc.Initialize()) {
    why = "That file isn't an Xbox 360 disc image bardown can read.";
    return false;
  }
  auto* root = disc.ResolvePath("");
  if (!root) {
    why = "The disc image has no files bardown can find.";
    return false;
  }
  uint64_t total = 0, done = 0;
  std::function<void(rex::filesystem::Entry*)> measure = [&](rex::filesystem::Entry* e) {
    for (auto& c : e->children()) {
      if (c->attributes() & rex::filesystem::kFileAttributeDirectory) measure(c.get());
      else total += c->size();
    }
  };
  measure(root);
  // an entry name has to be a plain name, so a crafted image can't write outside dest
  auto safe = [](const std::string& n) {
    return !n.empty() && n != "." && n != ".." && n.find_first_of("/\\:") == std::string::npos;
  };
  std::vector<uint8_t> buf(4 << 20);
  std::function<bool(rex::filesystem::Entry*, const std::filesystem::path&)> copy =
      [&](rex::filesystem::Entry* e, const std::filesystem::path& to) {
        std::error_code ec;
        std::filesystem::create_directories(to, ec);
        for (auto& c : e->children()) {
          if (!safe(c->name())) {
            why = "The disc image has a file named '" + c->name() + "', which bardown won't extract.";
            return false;
          }
          if (c->attributes() & rex::filesystem::kFileAttributeDirectory) {
            if (!copy(c.get(), to / c->name())) return false;
            continue;
          }
          rex::filesystem::File* f = nullptr;
          if (c->Open(rex::filesystem::FileAccess::kGenericRead, &f) != 0 || !f) {
            why = "Couldn't read " + c->path() + " from the disc image.";
            return false;
          }
          std::ofstream out(to / c->name(), std::ios::binary);
          for (size_t off = 0; off < c->size();) {
            size_t got = 0;
            const size_t want = std::min(buf.size(), c->size() - off);
            if (f->ReadSync(std::span<uint8_t>(buf.data(), want), off, &got) != 0 || got == 0) break;
            out.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(got));
            off += got;
            done += got;
            progress = total ? float(double(done) / double(total)) : 1.0f;
          }
          f->Destroy();
          if (!out) {
            why = "Couldn't write " + (to / c->name()).string() + ". Is the disk full?";
            return false;
          }
        }
        return true;
      };
  return copy(root, dest);
}

// beside the program: an extracted game folder (one with default.xex in it, up to one level down), else the
// first .iso; whichever's found, or empty
struct Found {
  std::filesystem::path folder, image;
};
inline Found FindBeside(const std::filesystem::path& dir) {
  Found f;
  std::error_code ec;
  if (std::filesystem::is_regular_file(dir / "game" / "default.xex", ec)) {
    f.folder = dir / "game";
    return f;
  }
  for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.is_directory(ec) && std::filesystem::is_regular_file(e.path() / "default.xex", ec)) f.folder = e.path();
    auto ext = e.path().extension().string();
    for (auto& ch : ext) ch = char(std::tolower(uint8_t(ch)));
    if (e.is_regular_file(ec) && ext == ".iso" && f.image.empty()) f.image = e.path();
  }
  return f;
}

// records just the game folder in the settings file, leaving the rest of it as it was (saving every setting
// would also keep whatever the program was started with, a muted test run's audio_mute among them)
inline void SaveGameFolder(const std::filesystem::path& config, const std::filesystem::path& dir) {
  toml::table t;
  std::error_code ec;
  if (std::filesystem::exists(config, ec)) {
    try {
      t = toml::parse_file(config.string());
    } catch (const toml::parse_error&) {
    }
  }
  // inside the settings file's folder it's saved relative to it, so the whole folder can be moved
  const auto base = config.parent_path();
  const auto rel = dir.lexically_normal().lexically_relative(base.lexically_normal());
  const bool inside = !rel.empty() && *rel.begin() != "..";
  t.insert_or_assign("game_data_root", (inside ? rel : dir).generic_string());
  std::ofstream(config) << t << "\n";
}

}  // namespace first_run

// the setup window; calls done(folder) once it has a folder with the right game in it
class FirstRunDialog : public rex::ui::ImGuiDialog {
 public:
  // found: a disc image found beside the program, extracted straight away without asking
  FirstRunDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path data_dir, std::string problem,
                 std::function<void(std::filesystem::path)> done, std::filesystem::path found = {})
      : ImGuiDialog(drawer), data_dir_(std::move(data_dir)), message_(std::move(problem)), done_(std::move(done)) {
    if (!found.empty()) {
      picked_ = found.string();
      has_pick_ = true;
      picking_ = true;
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    Poll();
    TakePick();
    if (finished_) return;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, io.DisplaySize.x * 0.9f), 0));
    ImGui::Begin("bardown setup", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextWrapped(
        "bardown runs your own copy of NHL 14 for the Xbox 360. Put the disc image (.iso) next to bardown and start "
        "it again, or point it at the game here: the folder you extracted it to (the one with default.xex in it), or "
        "the disc image. A disc image is extracted once into %s.",
        data_dir_.string().c_str());
    ImGui::Spacing();
    if (extracting_) {
      ImGui::TextUnformatted("Extracting the disc image (once, a few minutes)...");
      ImGui::ProgressBar(progress_.load(), ImVec2(-1, 0));
    } else {
      if (ImGui::Button("Choose the game folder...")) Pick(true);
      ImGui::SameLine();
      if (ImGui::Button("Choose a disc image (.iso)...")) Pick(false);
      ImGui::SameLine();
      if (ImGui::Button("Quit")) std::exit(0);
    }
    {
      std::lock_guard lock(mutex_);
      if (!message_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", message_.c_str());
        ImGui::PopStyleColor();
      }
    }
    ImGui::End();
  }

 private:
  void Pick(bool folder) {
    if (picking_.exchange(true)) return;
    auto cb = [](void* self, const char* const* list, int) {
      auto* d = static_cast<FirstRunDialog*>(self);
      std::lock_guard lock(d->mutex_);
      d->picked_ = list && list[0] ? std::string(list[0]) : std::string();
      d->picked_folder_ = d->picking_folder_;
      d->has_pick_ = true;
    };
    picking_folder_ = folder;
    if (folder) {
      SDL_ShowOpenFolderDialog(cb, this, nullptr, nullptr, false);
    } else {
      static const SDL_DialogFileFilter kIso[] = {{"Xbox 360 disc image", "iso"}};
      SDL_ShowOpenFileDialog(cb, this, nullptr, kIso, 1, nullptr, false);
    }
  }

  // a path the system's picker came back with
  void TakePick() {
    std::string path;
    bool folder;
    {
      std::lock_guard lock(mutex_);
      if (!has_pick_) return;
      has_pick_ = false;
      picking_ = false;
      path = picked_;
      folder = picked_folder_;
    }
    if (path.empty()) return;  // cancelled
    if (folder) {
      Accept(path);
      return;
    }
    // an image: out into our own folder, then the same check
    extracting_ = true;
    progress_ = 0;
    std::thread([this, iso = std::filesystem::path(path)] {
      const auto dest = data_dir_ / "game";
      std::string why;
      const bool ok = first_run::ExtractImage(iso, dest, progress_, why);
      extracting_ = false;
      if (!ok) {
        std::lock_guard lock(mutex_);
        message_ = why;
        return;
      }
      std::lock_guard lock(mutex_);
      accept_after_extract_ = dest;
    }).detach();
  }

  void Accept(const std::filesystem::path& dir) {
    const std::string why = first_run::CheckGameFolder(dir);
    if (!why.empty()) {
      std::lock_guard lock(mutex_);
      message_ = why;
      return;
    }
    finished_ = true;
    REXLOG_INFO("[bardown] setup: game folder {}", dir.string());
    done_(dir);
    Close();
  }

  // the extraction thread hands its folder back here, on the UI thread
  void Poll() {
    std::filesystem::path dir;
    {
      std::lock_guard lock(mutex_);
      if (accept_after_extract_.empty()) return;
      dir = std::move(accept_after_extract_);
      accept_after_extract_.clear();
    }
    Accept(dir);
  }

  std::filesystem::path data_dir_;
  std::string message_;
  std::function<void(std::filesystem::path)> done_;
  std::mutex mutex_;
  std::string picked_;
  bool picked_folder_ = false, picking_folder_ = false, has_pick_ = false;
  std::atomic<bool> picking_{false}, extracting_{false};
  std::atomic<float> progress_{0};
  std::filesystem::path accept_after_extract_;
  bool finished_ = false;
};

}  // namespace bardown

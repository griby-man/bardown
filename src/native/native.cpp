// the native renderer (docs/RENDERER.md): the game's Direct3D calls drawn with Vulkan, with shaders translated
// from the player's disc. on by default; with native_renderer off everything here passes straight through and
// the emulated GPU draws as before

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL_messagebox.h>

#include "guest.h"
#include "native.h"
#include "render.h"
#include "streams.h"
#include "textures.h"
#include "vk.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/ui/external_output.h>

#define XXH_INLINE_ALL
#include <xxhash.h>


REXCVAR_DEFINE_BOOL(native_renderer, true, "GPU/Native",
                    "Draw with the native renderer; off falls back to emulating the Xbox GPU")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_INT32(native_resolution_scale, 2, "GPU/Native",
                     "The native renderer draws at the Xbox's 1280x720 times this")
    .range(1, 3)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(native_shader_cache, "", "GPU/Native",
                      "The translated shaders (shaders.bin); empty looks beside the settings file");

REX_EXTERN(__imp__sub_827D9E48);  // Direct3D_CreateDevice
REX_EXTERN(__imp__sub_827E8B08);  // D3DDevice_Swap
REX_EXTERN(__imp__sub_827DDAE8);  // CreateVertexShader
REX_EXTERN(__imp__sub_827DD708);  // CreatePixelShader

namespace bardown::native {

namespace {

// shaders.bin, written by XenosRecomp from the disc: "BDSC", version 6, count, then per shader {u64 hash of
// the guest shader container, u32 offset, u32 size, u32 spec constants mask, u32 0}, then plain SPIR-V
struct Shader {
  uint32_t offset, size, spec, flags;
};
struct ShaderCache {
  std::vector<uint8_t> spirv;
  std::unordered_map<uint64_t, Shader> by_hash;
};

bool LoadShaders(const std::filesystem::path& path, ShaderCache& out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  uint32_t head[3];
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head)) || head[0] != 0x43534442 || head[1] != 10) return false;
  std::vector<uint8_t> table(size_t(head[2]) * 24);
  if (!in.read(reinterpret_cast<char*>(table.data()), std::streamsize(table.size()))) return false;
  out.spirv.assign(std::istreambuf_iterator<char>(in), {});
  for (uint32_t i = 0; i < head[2]; ++i) {
    const uint8_t* e = table.data() + size_t(i) * 24;
    uint64_t hash;
    Shader s;
    std::memcpy(&hash, e, 8);
    std::memcpy(&s.offset, e + 8, 4);
    std::memcpy(&s.size, e + 12, 4);
    std::memcpy(&s.spec, e + 16, 4);
    std::memcpy(&s.flags, e + 20, 4);
    if (uint64_t(s.offset) + s.size > out.spirv.size()) return false;
    out.by_hash.emplace(hash, s);
  }
  return true;
}

// the translator: next to the program when packaged, else in the build tree; with the header its shaders include
struct Translator {
  std::filesystem::path tool, header;
};
std::optional<Translator> FindTranslator() {
  const auto exe = rex::filesystem::GetExecutableFolder();
#ifdef _WIN32
  constexpr const char* kExt = ".exe";
#else
  constexpr const char* kExt = "";
#endif
  const Translator candidates[] = {
      {exe / (std::string("xenosrecomp") + kExt), exe / "shader_common.h"},
      {exe / (std::string("../xenosrecomp/XenosRecomp/XenosRecomp") + kExt),
       exe / "../../../third_party/XenosRecomp/XenosRecomp/shader_common.h"},
  };
  std::error_code ec;
  const auto* found = std::find_if(std::begin(candidates), std::end(candidates), [&](const Translator& c) {
    return std::filesystem::exists(c.tool, ec) && std::filesystem::exists(c.header, ec);
  });
  if (found == std::end(candidates)) return std::nullopt;
  return Translator{std::filesystem::canonical(found->tool, ec), std::filesystem::canonical(found->header, ec)};
}

// runs the translator and waits for it: its exit code, or -1 if it didn't start
int RunTranslator([[maybe_unused]] const Translator& t, const std::vector<std::filesystem::path>& args) {
#ifdef _WIN32
  std::wstring line;
  for (const auto& a : args) line += (line.empty() ? L"\"" : L" \"") + a.wstring() + L"\"";
  // the compiler dll beside the translator is found from its own folder
  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    REXLOG_WARN("[native] couldn't start the shader translator");
    return -1;
  }
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return int(code);
#else
  std::vector<std::string> strings(args.size());
  std::transform(args.begin(), args.end(), strings.begin(), [](const std::filesystem::path& a) { return a.string(); });
  std::vector<char*> argv(strings.size() + 1, nullptr);
  // cppcheck-suppress constParameterReference ; posix_spawn wants char*, which a const string won't give
  std::transform(strings.begin(), strings.end(), argv.begin(), [](std::string& a) { return a.data(); });
  // the compiler library sits beside the translator
#ifdef __APPLE__
  const std::string path_var = "DYLD_LIBRARY_PATH=";
  char** const host_env = *_NSGetEnviron();
#else
  const std::string path_var = "LD_LIBRARY_PATH=";
  char** const host_env = environ;
#endif
  // ahead of whatever the path already holds (an AppImage's own C++ runtime, on an older distro)
  std::string path = path_var + t.tool.parent_path().string();
  std::vector<std::string> env;
  for (char** e = host_env; *e; ++e) {
    if (std::strncmp(*e, path_var.c_str(), path_var.size()) == 0) {
      if ((*e)[path_var.size()]) path += std::string(":") + (*e + path_var.size());
    } else {
      env.emplace_back(*e);
    }
  }
  env.push_back(path);
  std::vector<char*> envp(env.size() + 1, nullptr);
  // cppcheck-suppress constParameterReference ; posix_spawn wants char*, which a const string won't give
  std::transform(env.begin(), env.end(), envp.begin(), [](std::string& e) { return e.data(); });
  pid_t pid;
  if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), envp.data()) != 0) {
    REXLOG_WARN("[native] couldn't start the shader translator");
    return -1;
  }
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// translates the disc's shaders into the cache: written beside it, then renamed in, so a cut-off run never leaves
// half a cache. in the background while the game plays on the emulated GPU; with no emulated GPU (wait) there's
// nothing else to draw the game, so startup waits for it
void Translate(const std::filesystem::path& game, const std::filesystem::path& cache, bool wait) {
  const auto t = FindTranslator();
  if (!t) {
    REXLOG_WARN("[native] the shader translator isn't next to bardown; can't make {}", cache.string());
    return;
  }
  auto work = [t = *t, game, cache, wait] {
    // the translator picks its output format by the name ending in .bin
    auto part = cache;
    part.replace_extension(".part.bin");
    REXLOG_INFO("[native] translating the disc's shaders into {} (a few minutes{})", cache.string(),
                wait ? "" : ", in the background");
    const int status = RunTranslator(t, {t.tool, game, part, t.header});
    std::error_code ec;
    if (status == 0 && std::filesystem::file_size(part, ec) > 12) {
      std::filesystem::rename(part, cache, ec);
      REXLOG_INFO("[native] shaders translated{}", wait ? "" : "; the native renderer can use them from the next start");
    } else {
      std::filesystem::remove(part, ec);
      REXLOG_WARN("[native] shader translation failed (status {})", status);
      // with no emulated GPU there's nothing else to draw the game
      if (wait)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "bardown",
                                 "The game's shaders couldn't be translated, so nothing can be drawn. The log in "
                                 "the logs folder says why.\n\nSetting native_renderer = false in bardown.toml "
                                 "uses the emulated GPU instead.",
                                 nullptr);
    }
  };
  if (wait)
    work();
  else
    std::thread(work).detach();
}

struct State {
  bool on = false;
  bool drawing = false;  // the shaders are in
  bool ready = false;    // and the device is set up
  rex::Runtime* runtime = nullptr;
  ShaderCache shaders;
  std::mutex mutex;
  std::unordered_map<uint32_t, uint32_t> devices;  // guest device -> devtype
  std::unordered_map<uint32_t, uint64_t> shader_hash;  // guest shader object -> its container's hash
  uint32_t shaders_found = 0, shaders_missing = 0;
  // BARDOWN_NATIVE_PROBE=<swap>:<draws> logs that many draws in detail from that frame on
  uint64_t probe_from = 0, probe_left = 0;
  uint64_t swaps = 0;
  std::chrono::steady_clock::time_point last_swap;
  double last_present = 0, worst_gap = 0, worst_present = 0;
  uint32_t slow = 0;
};
State& Get() {
  static State s;
  return s;
}

}  // namespace

// at startup, with the settings and the player's folder known
void Start(rex::Runtime* runtime, const std::filesystem::path& folder, const std::filesystem::path& game) {
  State& s = Get();
  s.on = REXCVAR_GET(native_renderer);
  s.runtime = runtime;
  if (const char* probe = std::getenv("BARDOWN_NATIVE_PROBE")) {
    unsigned long long from = 0, left = 0;
    if (std::sscanf(probe, "%llu:%llu", &from, &left) == 2) {
      s.probe_from = from;
      s.probe_left = left;
    }
  }
  if (!s.on) return;
  std::filesystem::path cache = REXCVAR_GET(native_shader_cache);
  if (cache.empty()) cache = folder / "shaders.bin";
  if (!LoadShaders(cache, s.shaders)) {
    const bool null_gpu = NullGpu();
    Translate(game, cache, null_gpu);
    if (!null_gpu || !LoadShaders(cache, s.shaders)) return;
  }
  REXLOG_INFO("[native] {} shaders loaded from {} ({} MB of SPIR-V); {}", s.shaders.by_hash.size(), cache.string(),
              s.shaders.spirv.size() >> 20, NullGpu() ? "no GPU emulation" : "the emulated GPU runs underneath");
  s.drawing = true;
}

}  // namespace bardown::native

using bardown::native::Get;

// the game makes its devices here: the real one, and the recording devices its worker threads draw with
extern "C" REX_FUNC(sub_827D9E48) {
  const uint32_t devtype = ctx.r4.u32, out = ctx.r8.u32;  // r8: where the new device's address goes
  __imp__sub_827D9E48(ctx, base);
  auto& s = Get();
  if (!s.on || !out) return;
  uint32_t device;
  std::memcpy(&device, bardown::native::guest::Host(base, out), 4);
  device = __builtin_bswap32(device);
  std::lock_guard lock(s.mutex);
  REXLOG_INFO("[native] device {:08X} created, type {} ({})", device, devtype, devtype == 1 ? "the real one" : "recording");
  s.devices[device] = devtype;
}

extern "C" REX_FUNC(sub_827E8B08) {
  const uint32_t front = ctx.r4.u32;
  __imp__sub_827E8B08(ctx, base);
  auto& s = Get();
  if (!s.on) return;
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  double present_ms = 0;
  if (s.drawing) {
    // the device is only ready once the game is presenting
    static const bool ready = bardown::native::RenderInit(s.runtime, uint32_t(REXCVAR_GET(native_resolution_scale)));
    if (ready && s.ready) {
      // the front buffer is a texture the game resolved the frame into
      namespace g = bardown::native::guest;
      const uint32_t address = front ? g::PhysicalAddress(g::U32(base, front + 32) & 0xFFFFF000u) : 0;
      bardown::native::RenderPresent(bardown::native::SwapImage(address));
      rex::ui::ExternalFrameDone();
      present_ms = std::chrono::duration<double, std::milli>(clock::now() - now).count();
    }
    s.ready = ready;
  }
  // frame pacing, so a hitch in a normal run says where it came from
  if (s.last_swap != clock::time_point{}) {
    const double gap = std::chrono::duration<double, std::milli>(now - s.last_swap).count();
    s.slow += gap > 20;
    if (gap > s.worst_gap) {
      s.worst_gap = gap;
      s.worst_present = s.last_present;
    }
  }
  s.last_swap = now;
  s.last_present = present_ms;
  if (++s.swaps % 600 == 0) {
    double gpu_wait = 0, gpu_worst = 0;
    bardown::native::TakeGpuWait(gpu_wait, gpu_worst);
    REXLOG_INFO("[native] pacing: {} frames over 20 ms, worst {:.1f} ms (presenting the frame before it took {:.1f}); "
                "waited on the gpu {:.0f} ms in all, {:.1f} at most; {} cached vertex streams changed; {} late textures "
                "changed by the swap",
                s.slow, s.worst_gap, s.worst_present, gpu_wait, gpu_worst, bardown::native::TakeStreamChanges(),
                bardown::native::TakeLateChanges());
    s.slow = 0;
    s.worst_gap = 0;
    uint64_t draws = 0, skipped = 0;
    bardown::native::DrawStats(draws, skipped);
    std::lock_guard lock(s.mutex);
    REXLOG_INFO("[native] {} frames swapped; shaders made {}, {} of them translated; {} draws, {} skipped; {} "
                "textures loaded; peak upload {} MB a frame",
                s.swaps, s.shaders_found + s.shaders_missing, s.shaders_found, draws, skipped,
                bardown::native::TextureLoads(), bardown::native::TakePeakUpload() >> 20);
  }
}

namespace {

// the blob the game hands over is the same container the translator found on the disc: flags, the size of the
// header part, the size of the microcode after it. hashed the same way, it names the translated shader
void OnShaderCreated(const PPCContext& ctx, uint8_t* base, uint32_t blob, const char* kind) {
  auto& s = Get();
  const uint32_t shader = ctx.r3.u32;
  if (!s.on || !shader || !blob) return;
  uint32_t head[3];
  std::memcpy(head, bardown::native::guest::Host(base, blob), sizeof(head));
  const uint64_t size = uint64_t(__builtin_bswap32(head[1])) + __builtin_bswap32(head[2]);
  const uint64_t hash = XXH3_64bits(bardown::native::guest::Host(base, blob), size);
  std::lock_guard lock(s.mutex);
  s.shader_hash[shader] = hash;
  if (s.shaders.by_hash.count(hash)) {
    ++s.shaders_found;
  } else {
    ++s.shaders_missing;
    REXLOG_WARN("[native] {} shader {:016X} ({} bytes) isn't in the cache", kind, hash, size);
  }
}

}  // namespace

extern "C" REX_FUNC(sub_827DDAE8) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_827DDAE8(ctx, base);
  OnShaderCreated(ctx, base, blob, "vertex");
}

extern "C" REX_FUNC(sub_827DD708) {
  const uint32_t blob = ctx.r3.u32;
  __imp__sub_827DD708(ctx, base);
  OnShaderCreated(ctx, base, blob, "pixel");
}

namespace {

// one draw, everything the native side has to reproduce
void Probe(const uint8_t* base, uint32_t dev, const char* kind, uint32_t prim, uint32_t a, uint32_t b, uint32_t c) {
  namespace g = bardown::native::guest;
  auto& s = Get();
  if (!s.probe_left) return;
  std::lock_guard lock(s.mutex);
  if (!s.probe_left || s.swaps < s.probe_from) return;
  --s.probe_left;
  const uint32_t vs = g::U32(base, dev + g::kVertexShader), ps = g::U32(base, dev + g::kPixelShader);
  const auto hash = [&](uint32_t shader) { auto it = s.shader_hash.find(shader); return it == s.shader_hash.end() ? 0 : it->second; };
  REXLOG_INFO("[probe] frame {} dev {:08X} {} prim {} args {} {} {} vs {:016X} ps {:016X}", s.swaps, dev, kind, prim, a, b,
              c, hash(vs), hash(ps));
  const uint32_t decl = g::U32(base, dev + g::kDecl);
  if (decl) {
    const uint32_t n = g::U32(base, decl + 24);
    for (uint32_t i = 0; i < n && i < 16; ++i) {
      const uint32_t e = decl + 52 + 12 * i;
      const uint32_t stream = g::U16(base, e), offset = g::U16(base, e + 2), type = g::U32(base, e + 4);
      const uint32_t fetch = dev + g::kFetch - 0x480 + g::StreamFetch(stream);
      REXLOG_INFO("[probe]   in s{} +{} type {:06X} usage {} {} | fetch {:08X} {:08X} stride {}", stream, offset, type,
                  g::Byte(base, e + 9), g::Byte(base, e + 10), g::U32(base, fetch), g::U32(base, fetch + 4),
                  g::Byte(base, dev + g::kStreamStride + stream) * 4);
    }
  }
  const uint32_t ib = g::U32(base, dev + g::kIndices);
  if (ib) REXLOG_INFO("[probe]   ib {:08X} addr {:08X} common {:08X}", ib, g::U32(base, ib + 24), g::U32(base, ib));
  const uint32_t rt = g::U32(base, dev + g::kTargets), ds = g::U32(base, dev + g::kDepth);
  REXLOG_INFO("[probe]   rt {:08X} [{:08X} {:08X} {:08X}] ds {:08X} surf {:08X} color {:08X} depth {:08X}", rt,
              rt ? g::U32(base, rt + 24) : 0, rt ? g::U32(base, rt + 28) : 0, rt ? g::U32(base, rt + 36) : 0, ds,
              g::Reg(base, dev, 0x2000), g::Reg(base, dev, 0x2001), g::Reg(base, dev, 0x2002));
  float vp[6];
  for (int i = 0; i < 6; ++i) {
    const uint32_t v = g::Reg(base, dev, 0x210F + i);
    std::memcpy(&vp[i], &v, 4);
  }
  REXLOG_INFO("[probe]   depthctl {:08X} blend0 {:08X} colorctl {:08X} mode {:08X} vte {:08X} mask {:08X} "
              "vp {} {} {} {} {} {} sc {:08X} {:08X} wo {:08X}",
              g::Reg(base, dev, 0x2200), g::Reg(base, dev, 0x2201), g::Reg(base, dev, 0x2202),
              g::Reg(base, dev, 0x2205), g::Reg(base, dev, 0x2206), g::Reg(base, dev, 0x2104), vp[0], vp[1], vp[2],
              vp[3], vp[4], vp[5], g::Reg(base, dev, 0x2081), g::Reg(base, dev, 0x2082), g::Reg(base, dev, 0x2080));
  for (uint32_t t = 0; t < 26; ++t) {
    const uint32_t tex = g::U32(base, dev + g::kTextures + 4 * t);
    if (!tex) continue;
    const uint32_t f = dev + g::kFetch + 24 * t;
    REXLOG_INFO("[probe]   tex{} {:08X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", t, tex, g::U32(base, f),
                g::U32(base, f + 4), g::U32(base, f + 8), g::U32(base, f + 12), g::U32(base, f + 16),
                g::U32(base, f + 20));
  }
}

}  // namespace

namespace bardown::native {

uint64_t GuestShaderHash(uint32_t guest_shader) {
  auto& s = Get();
  std::lock_guard lock(s.mutex);
  auto it = s.shader_hash.find(guest_shader);
  return it == s.shader_hash.end() ? 0 : it->second;
}

bool ShaderSpirv(uint64_t hash, const uint32_t*& code, size_t& bytes, uint32_t& spec) {
  auto& s = Get();
  auto it = s.shaders.by_hash.find(hash);
  if (it == s.shaders.by_hash.end()) return false;
  code = reinterpret_cast<const uint32_t*>(s.shaders.spirv.data() + it->second.offset);
  bytes = it->second.size;
  spec = it->second.spec;
  return true;
}

bool CompositionShader(uint64_t hash) {
  auto& s = Get();
  auto it = s.shaders.by_hash.find(hash);
  return it != s.shaders.by_hash.end() && (it->second.flags & 1);
}

bool Drawing() { return Get().ready; }

uint32_t DeviceType(uint32_t device) {
  auto& s = Get();
  std::lock_guard lock(s.mutex);
  auto it = s.devices.find(device);
  return it == s.devices.end() ? 0 : it->second;
}

void ProbeDraw(const uint8_t* base, uint32_t device, const char* kind, uint32_t prim, uint32_t a, uint32_t b, uint32_t c) {
  Probe(base, device, kind, prim, a, b, c);
}

}  // namespace bardown::native

# Changelog

What changed, newest first. Dates are when it landed.

## 0.3.4 (2026-10-16)

- **Sounds start clean.** The game turns on a sound's decoder before handing it any audio, and the runtime
  decoded on the spot, found nothing, and flagged the output buffer. The game reads that flag as "buffer
  full", so every sound in the game started with 35 ms of whatever that buffer played last, and the real
  sound came in 35 ms late. On the pause menu that was a random swoosh, loud or silent, on every move. The
  flag now only clears once the decoder has written something, as on the hardware.
- **Sounds play to the end.** The decoder held back each sound's last 320 samples, waiting for a frame that
  never came; the game then busy-waited for them. They're now handed over when the sound runs out of input.
- **Faces, jerseys and banners.** Textures the game rewrote in place, and images the GPU drew into memory
  the game later reused, were kept stale: black faces, a jersey drawn on a face, flashing replay banners.
  Both are now dropped when their memory changes. Alpha test uses the game's comparison (it was always "at
  least"), which fixes cut-outs drawn inside out; the shader cache is rebuilt on first start.
- F8 saves the last 10 seconds of sound to the logs folder, for reporting what you heard.

## 0.3.3 (2026-10-13)

- **Timers keep time.** The runtime's timer thread waited by spinning, and fired the game's 1,000-a-second timer
  about half a millisecond late on average, often more than a millisecond. It now sleeps until the next timer is
  due: about 55 microseconds late on average, never over a millisecond, and a little less CPU.
- The log says what a long frame spent its time on (any frame over 40 ms: drawing, pipelines built, textures
  loaded), how often the speakers asked for sound and none was ready (`[audio]`), and how late the timers fire
  (`[timers]`), every 10 seconds.

## 0.3.2 (2026-10-13)

- **Vsync is on.** The window was shown in Vulkan's immediate mode with nothing pacing it, so it redrew
  thousands of times a second while the game made 60: the GPU sat at full clock (about 200 W on an RTX
  3090 at 4K, with coil whine). Frames now go out in step with the display; the same 4K game draws about
  10 W over an idle desktop. `vsync_display` turns it off.
- **Even motion.** The window was redrawn on its own loop rather than when the game finished a frame, so
  about once a second a frame was shown twice and the next one skipped: a small stall and jump in
  play. Each game frame is now shown once, as it's made (60 a second in play, 30 in menus and cutscenes,
  as on the Xbox). The log counts any repeated or skipped frames every 10 seconds (`[present]`).
- **Frame limit** in the settings (Display, `present_frame_limit`): off, 30, 60, 120, 144 or 240 frames a
  second sent to the screen.
- When the game isn't sending frames (the first start's shader translation, a loading screen that sticks),
  the settings overlay redraws about 120 times a second instead of as fast as the GPU allows.

## 0.3.1 (2026-10-13)

- **Goal nets are see-through again.** The game draws its nets with the Xbox's alpha-to-mask, which cuts the
  mesh's holes out of the texture as it draws; the native renderer didn't support it, so every net came out
  as a solid white sheet. It's now handled (with one sample per pixel it keeps the half-opaque texels and
  drops the rest), so the ice, the crease and the posts show through the mesh.

## 0.3.0 (2026-10-12)

- **Windows.** A Windows 10/11 build, `bardown-0.3.0-windows-x64.zip`: unpack it anywhere, put the disc
  image beside it, run `bardown.exe`. It brings Microsoft's C++ runtime with it, and keeps its settings and
  saves in its own folder, or in `%LOCALAPPDATA%\bardown` when that folder can't be written to. Two bugs
  only Windows had are fixed on the way: the shader translator read file sizes as 32-bit numbers and crashed
  on the disc's 2.8 GB archive, and the renderer read part of the game's memory without the 4 KB shift the
  Windows runtime gives it, which garbled the HUD and menus. Checked on a real Windows 11 PC.
- **One AppImage for older distros too.** Built on Ubuntu 22.04, it runs on glibc 2.35 and newer (Ubuntu
  22.04, Mint 21 and later) instead of needing a distro as new as Arch. It carries a newer C++ runtime and
  uses it only where the system's is older.
- **The folder can move.** The game folder is saved relative to bardown's own folder, and if a saved one
  is gone, bardown looks beside itself again before asking. Moving the whole folder somewhere else just works.
- **No GPU emulation with the native renderer.** A new graphics plugin, `rexgpu-native`, replaces the
  emulated GPU: it answers the game's fences, interrupts and swaps and draws nothing, and the native
  renderer draws everything, including the team-colour composition at load, which is now written back
  to memory natively. It's chosen automatically while `native_renderer` is on; turning that off loads
  the emulated GPU as before.
- Draws the game builds a vertex at a time go straight to the native renderer instead of through the
  Xbox's command stream, which halves what the game writes to it each frame.
- Players no longer vanish on alternate frames without the emulated GPU. Their bone matrices live in
  two textures the game rewrites in turn; one was trusted to a memory watch that missed the writes, and
  both were re-read a frame too early. Textures read by vertex shaders are now checked against memory
  each frame, when the draws that use them are replayed.
- With no shader cache yet, the first start waits for the translation instead of showing a black screen.
- **Older CPUs.** The game code is built for x86-64-v2 (SSE4.2) instead of x86-64-v3 (AVX2), so it
  starts on Intel CPUs from 2009 and AMD from 2011. Measured in a match on the test machine, the speed is
  the same.
- **System requirements** in the README: the hard requirements, a minimum and a recommended spec, and
  what the test machine measured.
- **GPUs without the Xbox's depth format.** The native renderer used 24-bit depth with stencil, which
  AMD and Apple GPUs don't offer in Vulkan. It now falls back to 32-bit float depth with stencil where
  the 24-bit format is missing (tested by forcing the fallback on NVIDIA; not yet on an AMD card).
- **A GPU that can't run the native renderer says so.** The features it needs are checked at startup;
  if any is missing, a message names them and points to `native_renderer = false`, instead of a black
  screen.
- **Groundwork for macOS** (not built yet): the POSIX-only calls have macOS versions, settings and logs
  go to Application Support, and a build workflow for GitHub's Mac runners is in `.github/workflows`.

## 0.2.1 (2026-10-12)

- **The AppImage starts again when you open it from a file manager.** With no log path given, the program
  tried to make a logs folder inside the AppImage itself, which is read-only, and quit on the spot. Logs
  now go in a `logs` folder beside the AppImage, one per start.

## 0.2.0 (2026-10-12)

- **The native renderer draws the game, and it's the default.** Menus and matches are drawn from the
  game's own Direct3D calls on Vulkan, with its shaders translated from your disc on first launch. In a
  match it holds 60 with the GPU at about half load, where the emulated path ran it at 80-90% and dipped.
  `native_renderer = false` goes back to the emulated GPU.
- What it took to match the emulated picture: team helmets and gear (built on the GPU at load, then
  compressed by the CPU, so those passes still go through the emulated GPU), player names on the
  jerseys, the replay transition, the light glints on the ice, and a crowd that flickered between two
  poses every frame. [docs/RENDERER.md](docs/RENDERER.md) has the cause of each.

## 0.1.0 (2026-10-11)

- **First release**: an AppImage for Linux. Drop your disc image next to it and run it.

- **First launch sets itself up.** Put your disc image next to bardown and it finds it, checks it's the
  right NHL 14, and extracts it once. If it finds nothing, a setup window asks. Packaged, settings,
  saves and the cache all live in one folder beside the program.
- **Sharper by default.** The game draws at twice the Xbox's resolution, 2560x1440, and holds close
  to 60.
- **Smooth at 60.** Frames that took twice as long (a hitch) went from 4% to under 0.7%. Two causes: a
  warning the GPU code wrote for every draw, up to 1,800 lines a second, now written once; and 1.8 GB a
  second of the game's own screen being copied back to memory nobody read, now skipped.
- **Settings you can read on a 4K screen.** The overlays use Roboto at a size matched to the screen, and
  the settings window opens at most of it, with room for the NHL 14 categories.
- **F1 opens the settings**, with an NHL 14 section holding every tuning value the game has, read live
  from the running game and saved between launches.
- **The second half of the season works.** The first intermission crashed on a function the scanner
  had missed. A new scan found 299 more like it, reached only through pointers; with them in,
  simulating a season or a Be a GM career no longer crashes either.
- The pad can be driven from a file while the game runs, for testing menus without a controller.
- More capture probes: presentation, cameras, controls, faceoffs, moves and the AI.

## 2026-10-09

- Frame bursts, and an animation log of every clip the game starts in a match.

## 2026-10-08

- Match logs: the puck, its owner and every player each tick, sounds and what triggered them, and the
  commentary's sentence choices.
- Matches with every team on the CPU.

## 2026-10-07

- Oracle harnesses that call the game's own code on recorded inputs: animation decoding, the skate
  model, lighting, puck flight and contact, possession, the release of shots and passes.
- Scripted analog input and a guest memory sampler.

## 2026-10-06

- Headless test runs inside gamescope, with frame capture, so testing never opens a window.
- A draw tracer for the GPU.

## 2026-10-05

- First boot. Title screen, then the 3D title screen, then the ice.
- Videos stopped showing green blocks; the lighting stopped being washed out; equipment kept its colours.
- Generated code built optimised, which also cured the hitching in cutscenes.
- A separate profile for automated runs, so testing never touches real saves.

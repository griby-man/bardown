# The renderer

bardown draws NHL 14 with its own Vulkan renderer. The game's Direct3D calls get hooked by address and
drawn natively, using the game's own shaders translated from your disc. Nothing emulates the Xbox GPU.

The game's Direct3D library is still the recompiled original. It keeps its device state, writes the Xbox
command stream and waits on fences like normal. A small plugin, `rexgpu-native`, answers the fences and
swaps and throws the rest away.

Unleashed Recompiled did the same kind of thing for Sonic Unleashed. This one was written for this game
and doesn't use their code.

The emulated GPU (the SDK's Xenia port) is still there as a fallback: `native_renderer = false`.

## Why

Emulating the 360's GPU means replaying its command stream and faking its EDRAM in shaders. At 2x on my
3090 that ran at 82% GPU in a match and peaked at 93%, which is where it dropped under 60. The native
renderer draws the same match at about half that.

## How it works

- The device keeps a shadow of the GPU state, and a lot of it gets written straight into that shadow by
  game code without calling any function. So at each draw I read the shadow (shaders, vertex layout,
  textures, blend, depth, stencil, viewport) and build a Vulkan pipeline from it, cached by state.
- In a match, three worker threads record about 99% of the draws (~950 a frame) and the main thread
  splices them in. I record each segment and replay the whole frame in the right order at the swap.
- Shaders: XenosRecomp turns the microcode into HLSL and DXC compiles it to SPIR-V. That happens on first
  start and lands in `shaders.bin` (2,118 shaders). They come from your disc, so they never ship.
- Render targets get resolved into textures (13 a frame in a match), which map to native copies. Team
  gear is composed on the GPU and then compressed by the game on the CPU, so those get read back into
  game memory at load.

## Stuff that bit me

Most of these looked fine in a still frame and only showed up in motion.

- Vertex shaders start with the vertex index in `r0.x`, and the crowd uses it to work out its instance.
  Without it the crowd was garbage.
- Vertex data needs swapping by the fetch's endian mode, and 16-bit elements need their halves swapped
  back on top of that.
- Float textures are always point-sampled on the 360. The crowd's bone palettes are float, and filtering
  them gave shaky, ghosting fans.
- Some textures get rewritten every frame (bone matrices, flipping between two). The write watch missed
  some of those writes, so the crowd flickered and players vanished on alternate frames. Those are now
  checked against memory every frame they're used.
- A resolved image could stay tied to an address after the game reused that memory, which made banners
  flash and portraits stretch.
- Alpha test only did "greater or equal". The game uses the other comparisons too.

## Debug switches

- `BARDOWN_NATIVE_TRACE=<frame>`: log every draw in a frame with its state
- `BARDOWN_NATIVE_FRAMES=<frame>:<n>`: save n frames in a row
- `BARDOWN_NATIVE_DUMP=<frame>`: dump per-draw constants (with the SDK's `BARDOWN_TRACE_SWAPS`, from both
  renderers)
- `BARDOWN_NATIVE_SHADOW=1`: run both renderers on the same frames (needs `--gpu_plugin=xenos`)
- `BARDOWN_NATIVE_RDOC=<frame>`: RenderDoc capture of one frame
- `BARDOWN_NATIVE_DEPTH32=1`: force the 32-bit depth fallback

# The native renderer

bardown draws NHL 14 with its own renderer. The game's Direct3D calls are intercepted and drawn with
Vulkan, using the game's own shaders translated from your disc. The Xbox's GPU isn't emulated.

The game code above that layer is untouched. NHL 14's Direct3D library is the recompiled original too:
it still keeps its device state, writes the Xbox's command stream and waits on GPU fences. A small
graphics plugin, `rexgpu-native`, answers those fences, interrupts and swaps and draws nothing.

The approach, drawing a recompiled game's Direct3D calls natively, is the one
[Unleashed Recompiled](https://github.com/hedge-dev/UnleashedRecomp) took for Sonic Unleashed. This
renderer was written for this game; no code was taken from it.

The emulated GPU, a port of Xenia's that the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)
carries, stays available as a fallback: `native_renderer = false` in `bardown.toml` loads it instead.

## Why

Emulating the Xbox's GPU means reading its command stream back command by command and reproducing its
on-chip framebuffer (EDRAM) in shaders. That's faithful, and expensive. At twice the Xbox's resolution
on an RTX 3090, the emulated path ran at 82% GPU load in a match on average and 93% at worst, which is
where frames dropped below 60. The native renderer draws the same match at about half that, with no
drops.

## What the game does

NHL 14 links the Xbox's Direct3D statically, at `0x827D0000`-`0x827FC200` in the executable
([D3D.md](D3D.md) maps it). The device works like any 360 game's: every setter writes into a shadow of
the GPU state inside the device object, and every draw flushes what changed into the command stream.

Three things make this game its own:

- **Drawing is split across devices.** The main thread owns the real device (clears, resolves, the swap).
  In a match, three "recording" devices on worker threads issue about 99% of the draws, roughly 950 a
  frame, and the main thread splices their recordings into its stream in order. The menus use the main
  device only.
- **A lot of state never goes through a function.** Sampler state is written straight into the device's
  fetch constants by game code, and render states go through a table in the device. The renderer reads
  the device's shadow at each draw instead of trusting the setters alone.
- **The game makes its own vertex and index buffers.** There's no create or lock call for them; they're
  found when they're bound.

Render targets are never sampled directly. The game resolves them into textures (13 resolves a frame in a
match), which maps onto a native copy.

## How it works

- **Hooks.** The game's Direct3D functions (draws, clears, resolves, the swap, the worker segments,
  predicated tiling) are replaced by address, the same way bardown swaps in its own code for other game
  functions.
- **Shaders.** [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) (MIT) turns Xbox shader microcode
  into HLSL, and DirectXShaderCompiler compiles that to SPIR-V. bardown runs the translator over your
  disc on the first start and keeps the result as `shaders.bin`: 2,118 shaders. They come from the disc,
  so they're never shipped.
- **Draws.** Each draw reads the device's shadow (shaders, vertex declaration and streams, textures,
  blend, depth, stencil, viewport and scissor) and becomes a Vulkan pipeline, cached by that state.
  Draws the game builds a vertex at a time get vertex space of the renderer's own, so they never reach
  the command stream.
- **The frame.** The main device's draws are recorded in order. Each worker segment is linked into the
  slot the main device left for it, and the whole frame is replayed in GPU order at the swap.
- **Graphics API.** Vulkan, on the runtime's own device, so the window, the overlays and the frame
  captures keep working.

## What it took

Each of these was found by putting the native renderer's inputs next to the emulated GPU's for the same
frames (per-draw constant dumps from both, in the same run) and by RenderDoc captures of single native
frames, not by guessing.

**Shaders.** With the translator's changes (`sdk/xenosrecomp.patch`), all 2,118 compile:

- vertex shaders that read textures (443 of them) read an explicit level, since a vertex shader has no
  derivatives to choose one; the 360 does the same;
- texture slots a shader uses without naming them (vertex texture fetch, slot 16 and up) are declared,
  and each table of per-draw constants has 32 slots so they don't run into the next;
- boolean constants missing from a shader's table use the same packed bit a declared one would;
- a vertex input listed at several fetch addresses is declared once;
- a 360 vertex shader starts with its vertex index in r0.x, and instanced meshes (the crowd above all)
  work out from it which instance they are; the translator hands it over;
- a fetch can ask for an explicit LOD, and pixel shaders work one out themselves with `getCompTexLOD`
  and hand it back with `setTexLOD`; the translator used to drop both, so jersey names sampled tiny mips
  and came out as blocks.

**Vertex data.** Words are swapped by the fetch's endian mode and read in the GPU's own component order
(the game packs colours as words, so byte order alone gets them wrong), and 16-bit elements have their
halves swapped back on top. Index buffers and texture headers keep virtual addresses, which have to be
made physical the way Direct3D does when it binds them. Streams whose indices run past their end (the
crowd's instancing) are repeated, so index i reads vertex i mod n as the shader's own fetch would.

**A match's own way of drawing.**

- **Inherited state.** A worker segment only sets what it doesn't inherit: vertex constants 0-15 (the
  camera) come from whatever the main device last gave the GPU. Constants are merged in that order, block
  by block.
- **Predicated tiling.** A match frame is too big for the 360's EDRAM, so it's drawn twice, once per
  half, and resolved in halves. Natively there's no EDRAM: a tiled target is one image, cleared once.
- **The viewport.** The translated vertex shaders already turn y over for Vulkan, so the game's viewport
  goes in with its y flipped back. The UI clips with one-bit stencil masks, which have to come from the
  game.

**Matching the emulated picture.** Each of these looked fine in still frames:

- **32-bit float textures** are point-sampled on the 360 whatever the fetch constant asks. The crowd's bone
  palettes are float, and filtering them blended neighbouring bones: shaking, ghosting spectators.
- **Mip layout.** Mip levels are placed with the emulator's layout code; an older routine got small and
  non-square mip tails wrong.
- **Resolved images read through another header.** A resolve writes a target in its destination header's
  channel order; an effect can read it back through a different swizzle (the replay transition uses one
  channel as a mask). Natively the image is read through the difference between the two.
- **Fetch tables for computed indices.** A declared element the shader only reads by computed index isn't an
  input once compiled, and was left out of the fetch table, so every glare sprite read the same particle
  and stacked into one white blob at centre ice.
- **Textures rewritten every frame.** The game animates the crowd and the players through bone matrices
  in textures it rewrites every frame, alternating between two. A memory watch missed the writes to some
  of them, which showed as a crowd flickering between two poses and, without the emulated GPU, players
  vanishing on alternate frames. Small linear textures and every texture a vertex shader reads are now
  compared with memory each frame. They're compared when the draws that use them are replayed, not when
  they're bound, because the worker threads record a frame ahead of the one being shown.

**Team gear.** Helmets, gloves and pants are composed on the GPU at load (template textures times team
colours), resolved to memory, and compressed to DXT by the CPU. With nothing reaching memory, helmets
came out black and green. The renderer now reads the composed target back from the GPU and writes it to
the game's memory in the layout the texture header asks for (tiled or linear, with its byte order), as
the 360's resolve would. It only happens at load,
so a match doesn't pay for it.

## Without the emulated GPU

With the native renderer drawing everything, what the emulated GPU still did was keep the game's
Direct3D library running: answering its fences and interrupts and counting its swaps. `rexgpu-native`
does that with the SDK's command-stream code and nothing else. It's about a sixth the size of the
emulator (1.1 MB against 6.6 MB), and bardown picks it whenever the native renderer is on.

Two things changed with it:

- **Pacing.** The plugin holds the stream at each swap until the native renderer has shown that frame
  (for at most 100 ms), so the game doesn't run ahead of what's on screen.
- **First start.** There's no emulated picture to fall back on while the shaders translate, so the first
  start waits for them.

Replacing the Direct3D library outright, rather than answering its command stream, was looked at and
left: measured in a match, the thread processing that stream uses 0.15% of the CPU.

## Debugging

Kept in for this kind of work:

- `BARDOWN_NATIVE_TRACE=<frame>` (with `BARDOWN_NATIVE_TRACE_FRAMES=<n>`) logs every draw in GPU order
  with its state, streams, fetch constants, constants hash and viewport;
- `BARDOWN_NATIVE_FRAMES=<frame>:<n>` writes n consecutive presented frames (timed screenshots skip
  frames and hide anything that flips each frame);
- `BARDOWN_NATIVE_DUMP=<frame>`, with the SDK's `BARDOWN_TRACE_SWAPS`, dumps per-draw constants from both
  renderers;
- `BARDOWN_NATIVE_SHADOW=1` runs both renderers on the same frames (with `--gpu_plugin=xenos`);
- `BARDOWN_NATIVE_RDOC=<frame>` takes a RenderDoc capture of one frame;
- `BARDOWN_NATIVE_DEPTH32=1` uses the 32-bit float depth fallback on a GPU that has the 24-bit format,
  to test it.

# NHL 14 XDK Direct3D map (bardown)

All addresses are guest addresses and match `sub_XXXXXXXX` in `generated/default`. Confidence tags:
**R** = read from the generated code, **M** = measured at runtime with the probe, **I** = inferred (XDK naming, matching against the Unleashed checklist, or shape of the code).

Calls per frame come from two headless, muted runs of the d3d probe (`src/d3d_capture.cpp`, below):
- **match**: an all-CPU match at 165-180 s, 763 swaps. With the probe on it ran at about 51 fps. Raw counts in `logs/d3dmap/counts_match.txt`.
- **menu**: Select Teams, then the loading screen, at 50-60 s, 299 swaps. Raw counts in `logs/d3dmap/fe/d3dcount.txt`.

Each count file lists, per function: total calls, calls per swap, up to 12 distinct callers (LR), up to 4 devices (r3), up to 4 threads, and 6 argument samples (r3-r10, f1).

## 0. Summary

- The D3D library is statically linked at **0x827D0000-0x827FC200**. The D3DX/XGraphics-style helpers continue to about 0x82810000.
- It is a standard 360 XDK device that keeps a shadow of the GPU state and flushes it at each draw:
  - every setter writes into the device struct and sets bits in 5 "pending" qwords;
  - every Draw* walks those bits and emits PM4 type-0 register runs plus constant uploads, then a `DRAW_INDX` packet.
- There are no precompiled or static PM4 command buffers in the game data path. Every draw goes through a D3D Draw* call (**M**: about 965 draws per frame in a match, against about 1800 GPU draws in RenderDoc, which includes the emulator's own resolve and clear passes).
- **The big complication: rendering is multithreaded across several D3D devices.**
  - One hardware device is used on the main thread: `0xADEC6A80` in both runs. It does Clear, Resolve and Swap.
  - Three "command-buffer" devices (`Direct3D_CreateDevice` with devtype 2, created by 82E2ED08) record on worker threads. In a match they issue about 99% of the DrawIndexedVertices calls.
  - Each worker records about 9.4 "render jobs" per frame (82E273D0). The main thread splices each recorded segment into its ring with `INDIRECT_BUFFER` packets (827E0B28/827E1200 and 827E18B0).
  - A translation layer must keep one native recorder per guest device and replay the segments in the order the main thread links them.
- Menus and UI are single-device: no job devices are active, and the UI draws with BeginVertices (84 per frame) and SetTexture (967 per frame).
- The game writes much of the state **inline into the device struct**, without calling a D3D function:
  - SetRenderState calls the setter through the device's function table;
  - sampler state is written straight into the fetch-constant shadow by game code (82E21840);
  - the shader constant setters are tiny memcpy functions.
- So a replacement renderer has to **read state from the guest device struct at draw time**, the way Unleashed reads the constants, rather than rely only on hooked setters.

## 1. Device object (size 0x6080; allocated 0x6100 and aligned to 128)

Created by `Direct3D_CreateDevice` 827D9E48 (**R**). The game keeps:
- the main device at `renderer+12`;
- the devtype-2 device at `renderer+16`.

Here `renderer` is the r3 object of 82720A28 (Present) and 827208B0 (create). The present parameters live at `renderer+3580`: 1280x720 by default (**R**).

| Offset | Contents | Conf |
|---|---|---|
| +0x00 u64 pending[0] | VS float const dirty bits; 1 bit = 4 float4 (64 B) | R |
| +0x08 u64 pending[1] | PS float const dirty bits | R |
| +0x10 u64 pending[2] | register blocks 0x2000/0x2100/0x2180/0x2200 plus shader/stream bits: bits 11-14 = shader change, (8<<16) = vertex fetch rebind, (16<<16) = PS | R |
| +0x18 u64 pending[3] | low 32 bits = fetch-constant slots, `(1<<63)>>(32+slot)`; high bits = block 0x2280 | R |
| +0x20 u64 pending[4] | blocks 0x2300/0x2380, bool/loop consts 0x4900, misc | R |
| +0x28 u64 | mask used with pending[2] for vertex-fetch rebinding (827EE828) | R |
| +0x30 (48) | ring write pointer; writes use `stwu`, so it points at the last dword written | R |
| +0x34 (52) | ring end | R |
| +0x38 (56) | kick threshold; past it, call 827E4828 (make room / kick, 68 callers) | R |
| +0x3C (60) | refcount (Release = 827D9FC0) | R |
| +0x40 | `setRenderState[101]` function table, index = D3DRS/4 | R |
| +0x1D4 | `setSamplerState[20]` table | R |
| +0x224 | `getRenderState[101]` table | R |
| +0x3B8 | `getSamplerState[20]` table | R |
| +0x480 | fetch constants: 32 slots x 24 B (6 dwords) = GPU regs 0x4800-0x48BF. Textures use slot = sampler; vertex streams use 2-dword fetches at the top of the area (stream s at 0x778-8*s, slot 31 for stream 0) | R |
| +0x780 | VS float constants c0-c255 (16 B each), uploaded to reg 0x4000 | R |
| +0x1780 | PS float constants c0-c255, uploaded to reg 0x4400 | R |
| +0x2780 | bool/loop constants, uploaded to reg 0x4900 | R |
| +0x2880..+0x2A70 | **GPU register shadow** in 8 blocks (offset → first reg x count): 0x2880→0x2000 x16, 0x28C0→0x2080 x3, 0x28CC→0x2100 x21, 0x2920→0x2180 x5, 0x2934→0x2200 x12, 0x2964→0x2280 x21, 0x29B8→0x2300 x38, 0x2A50→0x2380 x8. Checked against the setters: ZENABLE→RB_DEPTHCONTROL, COLORWRITEENABLE→RB_COLOR_MASK, ALPHAREF→RB_ALPHA_REF, viewport→PA_CL_VPORT_*, RT→RB_COLOR_INFO | R |
| +0x2B08 (11016) | owning thread (AcquireThreadOwnership) | R |
| +0x2B1C (11036) | current fence; resources get `res+8 = fence` when they are unbound | R |
| +0x2B3C..+0x2B40 | flag bytes (11068-11072): predicated tiling, etc. | R |
| +0x2FD0 (12240) | current vertex declaration | R |
| +0x2FE8+s (12264) | stride from the declaration per stream (bytes); +0x3268+s (12904) = stride from SetStreamSource/4 | R |
| +0x3004..0x3010 | COLORWRITEENABLE 0-3 cache | R |
| +0x3078..0x3084 | RS 308-320 cache (the RB_COLORn_INFO exp-bias state) | R |
| +0x320C (12812) | current index buffer | R |
| +0x3210 (12816) | render targets [4] | R |
| +0x3220 (12832) | depth-stencil surface | R |
| +0x3224 (12836) | stream sources [16?] (`(3209+s)*4`) | R |
| +0x3278 (12920) | textures [26+] (`(3230+s)*4`) | R |
| +0x32E0 (13024) | viewport floats x, y, w, h, then minZ at 0x32F0 and maxZ at 0x32F4 | R |
| +0x32FC (13052) | scissor RECT (l, t, r, b) | R |
| +0x330C (13068) | current pixel shader | R |
| +0x3310 (13072) | current vertex shader | R |
| +0x3330..0x3340 (13104) | default RT/DS pointers, compared with 0x3210.. to detect the back buffer | R |
| +0x3368.. | tiling rects (`count @13124`) for the predicated-tiling path | R |
| +0x3628/+0x3630 | deferred-release fence list (827DFD40) | R |
| +0x3B88 (15240) | front-buffer copy (52-byte texture header) used by Swap | R |

The "current device" pointer (used by `Resource::GetDevice` 827E2458 and by `DestroyResource`) is read via `*(0x820007B4)` or `*(0x820007B8)`, depending on `KeGetCurrentProcessType` (**R**).

### Render states

Static table at `0x83B17878`: 101 entries of `{get, set, default}`. Init (827EC258) copies each setter into `dev+0x40` and calls it with the default.
- D3DRS 0-36 are no-ops (8272DDD8).
- The full list of setters → registers is in `d3dmap.rs_map.txt`.
- Names checked against Unleashed's enum: 40 ZENABLE, 44 ZFUNC, 48 ZWRITEENABLE, 56 CULLMODE, 60 ALPHABLENDENABLE, 72 SRCBLEND, 76 DESTBLEND, 80 BLENDOP, 84-92 the alpha-blend trio, 96 ALPHATESTENABLE, 100 ALPHAREF, 200 SCISSORTESTENABLE, 204 SLOPESCALEDEPTHBIAS, 208 DEPTHBIAS, 212 COLORWRITEENABLE.
- The rest are known only by their target register: stencil → RB_DEPTHCONTROL/RB_STENCILREFMASK(_BF), 172 clip planes → PA_CL_CLIP_CNTL, 176-188 point size, 228-236 tessellation, 240-300 SQ_WRAPPING_0/1 (texture wrap per sampler), 308-320 RB_COLORn_INFO, 328 primitive reset enable, 332 reset index, 360-372 HiZ, 392/396 line.

How the game calls them:
- 82722620 = game `SetRenderState(renderer, state, value)`. It picks `renderer+16` (cmdbuf) or `+12` (main), calls through `dev+0x40+state`, and caches the value in the game table 0x83A460C8. About 1430 per frame.
- The 82E2 job code also calls setters directly (e.g. 827D7650, 827D6B48).

### Sampler states

Table at `0x83B17D38`: 20 entries `{get, set, default}`. Each setter edits fetch-constant words at `dev+0x480+24*sampler` and sets the `pending[3]` bit (**R**). The game also writes these words inline (82E21840).

## 2. API functions

Arguments follow the PPC ABI: r3 = device unless stated, r4.. in order, f1.. for floats. "/f" = calls per frame, match / menu.

| Function | Address | Args | Emits / does | /f match | /f menu | Conf |
|---|---|---|---|---|---|---|
| Direct3D_CreateDevice | 827D9E48 | r3 adapter, r4 devtype (1 = HW, 2 = command-buffer device), r5, r6 behavior flags (stored @+0x5F48), r7 present params, r8 ppDevice | alloc, ring 4800-byte bootstrap; HW path → 827ECCF8 → ME_INIT 827E4EC0; state init 827EC258 | 0 | 0 | R |
| Device Release | 827D9FC0 | r3 dev | atomic dec +60, teardown 827ECFE0 | ~0 | | R |
| AcquireThreadOwnership | 827DA048 | dev | +11016 = thread | 12.4 | | R/I name |
| ReleaseThreadOwnership | 827DA088 | dev | +11016 = 0 | 11.4 | | R/I name |
| SetRenderTarget | 827D9178 (827D9170 = `b` alias) | r4 index 0-3, r5 D3DSurface* or 0 | rt[i]; surface+28 → RB_COLOR_INFO/RB_COLORn_INFO shadow; RT0 → 827D6578 (RB_SURFACE_INFO and full-surface viewport/scissor) | 52 | 1.7 | R+M |
| SetDepthStencilSurface | 827D9508 | r4 surface | DS @+0x3220, RB_DEPTH_INFO | 32 | 4.8 | R+M |
| SetViewport | 827D8F40 | r4 D3DVIEWPORT9* {X, Y, W, H u32; MinZ, MaxZ f32} | → SetViewportF 827D68F0 (f1-f6) → PA_CL_VPORT_* shadow (0x210F-0x2114), window scissor 827D6608 | 67 | 14 | R+M |
| SetScissorRect | 827D8E48 | r4 RECT* | +0x32FC; PA_SC_WINDOW_SCISSOR_TL/BR via 827D6608 (type-0 0x2080 n=3, or SET_BIN_* packets when tiling) | 203 | 18 | R+M |
| SetTexture | 827DC7B8 | r4 sampler, r5 D3DBaseTexture* (0 = unbind), r6 pending mask | textures[s]; merges texture fetch const (tex+28..+48) into dev+0x480+24s, keeping the sampler bits; marks the old texture's fence | **1994** | 967 | R+M |
| SetStreamSource | 827D8FC0 | r4 stream, r5 D3DVertexBuffer*, r6 offset, r7 stride, r8 pending mask | vertex fetch @dev+0x778-8s = vb+24 (+offset), size = vb+28-offset; stride byte; stride change sets pending[2] bit 0x80000 | 1190 | 49 | R+M |
| SetIndices | 827D90E0 | r4 D3DIndexBuffer* | +0x320C | 761 | 2 | R+M |
| SetVertexDeclaration | 827DDE30 | r4 decl | +0x2FD0, pending[2] fetch-rebind bit | 255 | 1 | R+M (name I) |
| SetVertexShader | 827DDBD8 | r4 VS* | +0x3310; applies the shader's fetch/constant patch list (vs+872…) | 328 | 37 | R+M (VS/PS order I) |
| SetPixelShader | 827DD7F8 | r4 PS* | +0x330C; patch list at ps+40+[ps+60] copied into the fetch shadow | 328 | 37 | R+M |
| SetVertexShaderConstantF | 827DD300 | r4 start reg, r5 float4*, r6 count, r7 pending mask | memcpy → dev+0x780+16*reg; pending[0] \|= mask | 958 | 5 | R+M |
| SetPixelShaderConstantF | 827DD3D8 | same | → dev+0x1780+16*reg; pending[1] | 660 | 1.6 | R+M |
| GetPixelShader / GetVertexShader | 827DD9B8 / 827DDDA8 | r4 out | AddRef | | | R |
| Render-state setters | 827D6B48..827D8408 (table above) | r4 value | register shadow + pending bits | 15-160 each | | R+M |
| Sampler-state setters | 827D8418..827D8E30 | r4 sampler, r5 value | fetch-constant bits | up to 175 | | R+M |
| DrawIndexedVertices | 827E5BA0 | r4 D3DPRIMITIVETYPE (4 = tri list, 6 = tri strip, 13 = rect/quad), r5 BaseVertexIndex, r6 StartIndex, r7 IndexCount | full pending flush (§3), then type-0 0x2102 (VGT_INDX_OFFSET) = base vertex; `DRAW_INDX` (0xC0032201: viz, initiator, index base, size) from ib+24, index size from ib+0 bit31; splits at 65535; under tiling, wrapped in SET_BIN_MASK/COND_EXEC/EVENT_WRITE | **953** | 1.1 | R+M |
| DrawVertices | 827E5788 | r4 prim, r5 StartVertex, r6 VertexCount | flush; 0x2102 = start; `DRAW_INDX` (0xC0012201, auto index) | 11.5 | 0 | R+M |
| BeginVertices (DrawPrimitiveUP) | 827E52B8 | r4 prim, r5 vertex count, r6 stride | flush; reserves ring space (827E3790), writes fetch const 31 (0x48BA n=6) at the inline data, `DRAW_INDX`; returns r3 = pointer for the caller to fill | 68 | **84** | R+M |
| EndVertices | 827E5778 | dev | ring ptr = saved end | 1 (most callers skip it) | | R |
| Clear | 827DFAA0 | r4 Count, r5 pRects, r6 Flags (0x3F = all), r7 Color ARGB, f1 Z, r8 Stencil | → ClearF 827DFB60 → 827DF420 (internal clear shaders via IM_LOAD_IMMEDIATE, then a quad draw) / 827DEE80 (depth/HiZ) | 13 | 4 | R+M |
| ClearF | 827DFB60 | r4 flags, r5 rect, r6 float4 color*, f1 Z, r8 stencil | | 14 | | R |
| Resolve | 827E6760 | r4 Flags (low 3 bits: 0-3 = RT n, 4 = depth; 0x300 = also clear), r5 pSrcRect, r6 pDestTexture, r7 pDestPoint, r8 DestLevel, r9 DestSliceOrFace, r10 pClearColor, f1 ClearZ, stack: stencil, params | EDRAM→memory copy: RB_COPY_CONTROL 0x2318, RB_COPY_DEST_BASE 0x2319, RB_COPY_DEST_INFO 0x231B, resolve shaders via IM_LOAD_IMMEDIATE, fetch 0x4800, EVENT_WRITE, WAIT_REG_MEM | 13 | 3 | R+M |
| Swap | 827E8B08 | r4 front-buffer D3DTexture* | copies the 52-byte header to +15248, calls VdSwap (import 0x8396E3C4) | 1 | 1 | R+M |
| pre-present wait | 827E7DB8 | r4 0 | reg 0x0579, WAIT_REG_MEM; called just before Present's Resolve | 1 | | R, name unknown |
| SetShaderGPRAllocation? | 827DDFB8 | r4 flags, r5 VS GPRs, r6 PS GPRs (0, 0 → 64/64) | 3 IB packets to preset snippets + 827DCB20 | 7 | | R, name I |
| GPU cache event | 827DA130 | r4 | EVENT_WRITE 15 (+ reg 0x05C8 when r4 = 1); start of each job | 9.4 | | R, name unknown |
| CreateTexture | 827DC3F0 | **no device**: r3 W, r4 H, r5 Depth, r6 Levels, r7 Usage, r8 D3DFORMAT, r9 Pool, r10 ResourceType | 52-byte header (XGSetTextureHeader-like 827DBBE8) + physical alloc (828975B0) | setup only | | R |
| CreateSurface (EDRAM RT/DS) | 827DC510 | r3 W, r4 H, r5 format, r6 multisample, r7 D3DSURFACE_PARAMETERS* (0 = auto EDRAM base) | 48-byte header (827DBEE0), EDRAM allocator 827EE378 | setup only | | R |
| GetSurfaceLevel | 827DC160 | r3 texture, r4 level | 48-byte surface wrapping a texture level (+24 = parent) | ~1 | | R |
| LockRect (texture) | 827DB9D8 | r3 tex, r4 level, r5.. locked-rect out, flags | level offset 827DB558 → pointer/pitch | 1 | 1.8 | I |
| GetLevelDesc / GetDesc | 827DB358 / 827DB458 / 827DC678 | r3 res, r4/r5 desc out | | ~1 | | R |
| Resource flush (unlock) | 827E2148 | r3 resource | dcbst over base/mip ranges (+20/+24 BaseFlush/MipFlush); this is the signal that the CPU wrote resource memory | 12 | 4.3 | R |
| AddRef / Release | 827E2360 / 827E23D8 | r3 res | atomic +4; Release → DestroyResource 827E1AA0 at 0 | 2-3 | | R |
| GetType | 827E24D0 | r3 res | type from Common & 0xF (3 = texture family, 4 = surface) | 2 | | R |
| CreateVertexShader | 827DDAE8 | r3 microcode blob | 872-byte header + code; optional hook ptr at 0x83D6B4C4 (-19420) | load time | | R |
| CreatePixelShader | 827DD708 | r3 blob | 40-byte header + code; hook ptr at 0x83D6B4C8 | load time | | R |
| shader GetFunction | 827DD620 / 827DD678 | | copies microcode out; 827DE8E0 = hash of microcode (rotate-xor) | | | R |
| CreateVertexDeclaration | 827DDF38 | r3 D3DVERTEXELEMENT9[] (12 B each, end = stream 0xFF) | 56 + 12n bytes, init 827DDE48 | load time | | R |
| Segment begin (cmdbuf device) | 827E0038 | r3 job device, r4 segment… | points ring +48/52/56 at segment memory | 9.4 | | R, name I |
| Segment end | 827E05A8 | r3 job device | | 9.4 | | R, name I |
| Link segment into IB chain | 827E18B0 | r3 segment list obj | SET_BIN_SELECT_HI (0x63) + `INDIRECT_BUFFER` (0x3F) per recorded block | 9.4 | | R |
| Run segment on main device | 827E1200 (after 827E0B28) | r3 main dev, r4 segment | flushes main pending state, WAIT_REG_MEM 827E0E78, IB | 9.4 | | R, name I |
| RunCommandBuffer-style (unused) | 827DA2E0 | | IB + WAIT_REG_MEM | 0 | 0 | R |

Not present as D3D functions with game callers: **CreateVertexBuffer, CreateIndexBuffer, Lock/Unlock VB/IB**.
- The game fills the D3DVertexBuffer/IndexBuffer headers itself (XGSetVertexBufferHeader style, inlined or in game code) and writes the memory directly, then flushes it with 827E2148 (**I**: no allocator of those header sizes exists in the D3D range).

## 3. What a Draw* emits (the flush order, R)

`827E5BA0`, `827E5788` and `827E52B8` share the same inlined flush:

1. pending[0] → 827EEF50: VS consts, type-0 at 0x4000 + 4*bit, from dev+0x780.
2. pending[1] → 827EEF50: PS consts at 0x4400, from dev+0x1780.
3. pending[2]:
   - shaders (bits 11-14) → 827EFCE0: `IM_LOAD` (pointer shader upload), LOAD_ALU_CONSTANT 827EFAF0, SET_SHADER_BASES etc.;
   - fetch rebind → 827EE828;
   - then type-0 runs via 827EEBB8 for 0x2200 (dev+0x2934), 0x2180 (+0x2920), 0x2000 (+0x2880), 0x2100 (+0x28CC).
4. pending[3]: fetch constants via 827EEDF8 (type-0 at 0x4800 + 6*slot), then 0x2280 (+0x2964).
5. pending[4]: 0x2380 (+0x2A50), 827EEC78, bool/loop 0x4900 (+0x2780), 0x2300 (+0x29B8), 827D9D98.
6. VGT_INDX_OFFSET, then `DRAW_INDX`.

Under predicated tiling, 827D6450/827D6608 emit SET_BIN_MASK/SELECT (0x60-0x63) and COND_EXEC.

## 4. Resources (header layout)

All resources start with the XDK `D3DResource` header:

| Offset | Field |
|---|---|
| +0 | Common (type in the low nibble, flags in the high bits) |
| +4 | RefCount |
| +8 | Fence |
| +12 | ReadFence |
| +16 | Identifier |
| +20 | BaseFlush |

Per type (**R** where used):
- **Vertex buffer (type 1, I)**: +24 fetch dword0 (address \| type), +28 dword1 (size \| endian).
- **Index buffer (type 2, I)**: +24 physical address; +0 bit31 = 32-bit indices.
- **Texture (type 3; 52 B)**: +24 MipFlush; +28..+48 = 6-dword texture fetch constant (+32 base address/format, +36 size, +40 swizzle, +48 mip address).
- **Surface (type 4; 48 B)**: +24 RB_SURFACE_INFO-ish word (or parent texture for level surfaces), +28 RB_COLOR_INFO / RB_DEPTH_INFO (format + EDRAM base), +36 size.
- **Shaders**: microcode follows the header (VS at +872, PS at +40); +60 = offset of the fetch/constant patch list.

Address translation in fetch constants: `phys = ((addr>>20)+0x200 & 0x1000) + (addr & 0x1FFFFFFF)`, the 4 KB-page heap adjustment (**R**).

## 5. Resolves and render-target-as-texture

- Render targets are EDRAM surfaces (CreateSurface), never sampled directly.
- To use one as a texture, the game calls **Resolve(flags, rect, destTexture, …)**: 13 per frame in a match, 3 in menus. That copies EDRAM into the destination texture's memory (RB_COPY_*), and the texture is then bound with SetTexture like any other.
- Present (82720A28, the game's renderer vtable method):
  1. 827E7DB8(dev, 0);
  2. `Resolve(dev, 0x300, 0, frontTex=renderer+3704, …)`, or `Clear(…, 0x3F)` when told to;
  3. 827DDFB8(dev, 0, 0, 0);
  4. `Swap(dev, frontTex)`;
  5. kick 827E4828.
- A native renderer can map each (RT, destTexture) Resolve to a GPU copy or blit into a native texture, and key textures by guest base address so later SetTexture calls find them. This removes the EDRAM emulation that costs the fsi path its time.

## 6. Threading and the job system (R+M)

- **Creation.** 82E2ED08 creates devtype-2 devices (3 seen, plus the main one): `0xADBD2600`, `0xADBE4980`, `0xADBDE880`. They run on two worker threads plus the main thread.
- **Per-frame job.** About 9.4 per frame from 82E28DA8. Each job (82E273D0) runs:
  1. AcquireThreadOwnership;
  2. 82E24E20: segment begin 827E0038, cache event 827DA130, SetRenderTarget/DS/Viewport;
  3. **82E24F58**, the per-draw executor (~1490 per frame). Each call issues: RS setters, `Clear?`, SetViewport, SetScissorRect, sampler/texture setup (82E21790, 82E21840 writes fetch constants inline), SetVertexDeclaration, SetStreamSource, SetIndices, SetVertexShader, SetPixelShader, VS consts (82E219A8 → 827DD300), PS consts (82E21A40 → 827DD3D8), SetTexture, DrawVertices or (82E21AD8 → primitive reset RS 328/332 → DrawIndexedVertices);
  4. segment end 827E05A8;
  5. link 827E18B0;
  6. ReleaseThreadOwnership.
- **Splicing.** On the main thread, 82E21240 → 827E0B28 + 827E1200 splices each finished segment into the hardware ring.
- **A second hook layer.** 82E24F58 is a reasonable game-level hook point in its own right: one call per draw item, with r3 = job context and the device in the job. It is not decoded beyond the call list.

## 7. What a translation-layer renderer must replace, in order

Written before the renderer was. The renderer follows it, with two differences: the game's command stream and
fences are kept and answered by the `rexgpu-native` plugin rather than stubbed, and of resource creation only
the shaders are hooked (textures are found from their fetch constants at each draw).
[RENDERER.md](RENDERER.md) has what was built.

1. **Device creation.** Hook CreateDevice 827D9E48 for both devtypes.
   - Unleashed replaces the device with its own struct.
   - Here, keep the guest struct, so that the inline writes (RS table at +0x40, fetch shadow +0x480, consts +0x780/+0x1780, register shadow +0x2880) still land in it.
   - Point the RS/SS function tables at host setters, or leave them and read the shadow at draw time. Reading the shadow is less code and catches the inline sampler writes.
2. **Resource creation and lifetime.** CreateTexture 827DC3F0, CreateSurface 827DC510, GetSurfaceLevel 827DC160, CreateVertexShader/PixelShader 827DDAE8/827DD708 (translate the microcode ahead of time, keyed by the 827DE8E0 hash), CreateVertexDeclaration 827DDF38, DestroyResource 827E1AA0 / Release 827E23D8.
3. **CPU writes.** Hook resource flush 827E2148 (and LockRect 827DB9D8) to re-upload textures and buffers.
   - VB/IB have no Create call: discover them at SetStreamSource/SetIndices from the guest header (+24/+28) and cache them by address and size.
4. **Draw path.** DrawIndexedVertices 827E5BA0, DrawVertices 827E5788, BeginVertices/EndVertices 827E52B8/827E5778.
   - At each draw, consume the pending bits and translate the shadowed state: blend, depth, stencil, cull, viewport, scissor, fetch constants → samplers/textures/vertex layout, constants.
   - Hooking the setters (827DC7B8, 827D8FC0, 827D90E0, 827DDBD8, 827DD7F8, 827DDE30, 827DD300/827DD3D8, 827D8F40/827D68F0, 827D8E48) is optional, given the shadow.
5. **Targets.** SetRenderTarget 827D9178 / SetDepthStencilSurface 827D9508, Clear 827DFAA0/827DFB60, Resolve 827E6760.
6. **Multi-device recording.**
   - Per-device native command lists for the devtype-2 devices.
   - Hook segment begin/end 827E0038/827E05A8, and the main-thread splice 827E1200 (and 827E18B0), to submit recorded native lists in guest order.
   - Make sure the guest never builds PM4: return early from the Draw* hooks, and stub the IB linkage so the emulated GPU sees nothing.
7. **Present.** Swap 827E8B08 (present the native front buffer), plus 827E7DB8 and 827DDFB8 (stub or keep).
   - The fence/wait functions (827E4930 / 827D9A48 spin, 441k calls per frame in the match = the CPU waiting on the emulated GPU) must be satisfied by host fences, or stubbed.
8. **Fallback.** Leave everything else (ME_INIT, tiling, ring kicks) to the emulated path behind the setting.

## 8. Unverified

- VS vs PS identity of 827DDBD8/827DD7F8 is inferred from the fetch-rebind bit and the 872-byte VS header, not from shader contents.
- Resource type numbers 1 (VB) and 2 (IB); the VB/IB header layout beyond +24/+28.
- LockRect 827DB9D8 argument meaning.
- Names for 827DDFB8, 827DA130, 827E7DB8, 827E0038/827E05A8/827E1200/827E18B0 (behaviour read; XDK names guessed).
- RS names other than the 17 confirmed against Unleashed, and the "COHER_DEST_BASE_7(0x200D)" label for one RS 172 store (the SDK's name for 0x200D may not match this use).
- DrawIndexedVertices primitive value 13 seen from the overlay path (rect list?).
- Whether any frame in other modes (replays, NIS cutscenes, the Winter Classic) uses other D3D entry points: only a match and the menus were measured.
- Per-frame counts were taken with the probe on (about 51 fps). Counts are per swap and shouldn't depend on frame rate, but job counts could shift with timing.

## Files

- `src/d3d_capture.cpp`: the probe that produced the counts. It's kept out of the build; to use it, add it to
  `BARDOWN_SOURCES`, set `BARDOWN_D3D_PROBE=<dir> BARDOWN_D3D_FROM=<s> BARDOWN_D3D_FOR=<s>`, and run a headless
  match or menu session.
- `docs/d3d_render_states.txt`: all 101 render-state setters and 20 sampler-state setters with the GPU registers
  they write.
- Count files land in `logs/d3dmap/` (not in the repo).

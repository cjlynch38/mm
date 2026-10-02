# N64 display list renderer (Milestone 3)

The game builds N64 display lists every frame and submits them as an `M_GFXTASK` through
`osSpTaskStartGo` (`port/gc/ultra/sp.c`). On the N64, the RSP runs the F3DZEX2 microcode
(`gspF3DZEX2_NoN_PosLight_fifo`) over the list, and the RDP rasterizes into a 320x240 RGBA16
framebuffer. The VI then shows that buffer after `osViSwapBuffer`.

On the GameCube, `gc_gfx_run_task()` interprets the list on the CPU and draws through GX:

```
sp.c osSpTaskStartGo(M_GFXTASK) -> gc_gfx_run_task(data_ptr)        [Sched thread]
    gfx_gx_task_begin, gfx_rsp_reset, gfx_rdp_reset, gfx_tex_frame
    gfx_rsp_run(dl): RSP commands
        G_VTX     -> transform, light, fog, texgen into the vertex buffer (GfxVtx[32])
        G_TRI*    -> cull (winding), then gfx_gx_triangle
        G_MTX ... -> matrix stacks; gfx_gx_set_projection on projection changes
        RDP cmds  -> gfx_rdp_command / gfx_rdp_texrect -> gfx_gx_texrect / gfx_gx_fillrect
    gfx_gx_task_end: GX_DrawDone, EFB -> XFB slot for the frame's color image
    then sp.c posts OS_EVENT_SP / OS_EVENT_DP as before
vi.c retrace: current buffer changed -> gc_gfx_present(fb) -> VIDEO_SetNextFramebuffer(xfb[fb])
```

## Scope for the first pass

- **F3DZEX2 only.** The game switches to S2DEX2 (`G_LOAD_UCODE`) only for pre-rendered room
  backgrounds, the pause screen capture, `z_visfbuf.c`, one HUD path and the wipe5 transition. While
  S2DEX2 is loaded, its commands are skipped (logged once) until F3DZEX2 is loaded again.
- **One render target, the frame.** The first `G_SETCIMG` of a task is the frame. A later
  `G_SETCIMG` equal to the z image means fill rectangles clear depth (the N64 clears the z-buffer by
  filling it as a color image with `0xFFFC`). Any other color image is off-screen: its draws are
  skipped and logged once. Off-screen targets are framebuffer effects: motion blur, the pause
  background, the picture box, VisMono, Lens of Truth.
- **No CPU framebuffer or z-buffer readback.** The game's z-buffer reads return "far", as in M2.

## Vertex pipeline (gfx_rsp.c)

- Matrices: the N64 `Mtx` is 4x4 s15.16, stored as 16 integer halves followed by 16 fractional
  halves (row-major, `clip = v * M`). Modelview stack: 10 deep in F3DEX2, `G_MTX_PUSH`,
  `G_MTX_LOAD`/`MUL`. The projection matrix is a single matrix. MM puts projection times view into
  the projection matrix (`z_view.c`). Keep a cached modelview*projection product.
- `G_MW_FORCEMTX` / `G_MV_MATRIX` force the combined matrix.
- Lighting (`G_LIGHTING`): ambient plus directional lights. F3DZEX2 transforms each direction into
  model space through the modelview matrix (transpose). Positional lights (`G_LIGHTING_POSITIONAL`)
  are point lights with kc/kl/kq attenuation, the `PosLight` microcode variant.
  - Layouts: `Light_t` is col[3], pad, colc[3], pad, dir[3] (s8), pad. `PointLight_t` is col[3],
    kc, colc[3], kl, pos[3] (s16), kq.
  - `G_MV_LIGHT` offsets: lookat at 0x00/0x18, then lights at 0x18 * (n + 2).
  - `G_MW_NUMLIGHT` stores n*24.
- Texgen (`G_TEXTURE_GEN`, `_LINEAR`): spherical or linear environment mapping from the lookat
  vectors.
- Fog (`G_FOG`): the vertex alpha becomes the fog factor
  `clamp(fogMultiplier * (z/w) + fogOffset, 0, 255)`. Use the microcode's exact formula from z/w.
  The blender uses it (see gfx_tev.c).
- `G_SHADING_SMOOTH` off means flat shading: every vertex takes the first vertex's color.
- Culling (`G_CULL_FRONT`/`BACK`) uses the sign of the screen-space area, from NDC with y up.
- `G_CULLDL` uses vertex clip flags. `G_BRANCH_Z` compares the vertex's screen z (viewport-
  transformed, as the microcode computes it) with the given value.
- `G_MODIFYVTX` is only used with ST in MM; implement all fields.
- Texture coordinates: the `Vtx` s,t are s10.5. Output texel s,t = vtx.s * scaleS / 32 (and t
  likewise). With texgen, compute the microcode's values.

## Projection on GX (gfx_gx.c)

GX projection matrices cannot hold an arbitrary 4x4, but a vertex can be submitted in a form that
GX's own perspective divide turns back into the N64 clip position:

- **Perspective** (the projection's last column (m[0][3], m[1][3], m[2][3]) is non-zero):
  clip z is linear in clip w, z = a*w + b. Fit a and b from the matrix columns: column 2 equals a
  times column 3, plus b in the w row. Submit `(x, y, -w)`. Use a GX perspective matrix with
  p00 = p11 = 1, p02 = p12 = 0, and p22, p23 chosen so that GX's z/w equals the N64 depth mapped to
  GX's range. GX keeps z in [-w, 0]; the N64/OpenGL range is [-w, w].
- **Orthographic** (w is 1 for every vertex): submit `(x, y, z)` with a GX orthographic matrix that
  maps z from [-1, 1] to GX's range.
- **Other** (not expected): divide on the CPU and draw orthographically. Log it once.

The GX viewport comes from the N64 viewport times GFX_SCALE. Scissor comes from `G_SETSCISSOR`
times GFX_SCALE.

Decal z mode has no polygon offset on GX, so bias depth toward the viewer by a small constant.

As implemented (verified by port/gc/tests/gfx_gx_test, which reads the EFB back):
- GX clips z/w outside [-1, 0] and the viewport maps it to window depth (z/w) * (far - near) + far.
  Depth grows with distance, as on the N64: LEQUAL against a buffer cleared to GX_MAX_Z24.
- Perspective: submit `(x, y, -w)`; GX matrix p00 = p11 = 1, p22 = k (1 - a), p23 = k b, so GX's
  z_clip = k (z - w), with k = 1/8 (GX_ZK). The GX viewport's far comes from the N64 viewport z,
  far = (vtrans.z + vscale.z) / G_MAXZ (1022/1023 for the standard viewport), and
  far - near = vscale.z / (k G_MAXZ). GX window depth then equals the N64's
  (ndc * vscale.z + vtrans.z) / G_MAXZ exactly (the test measures 0 difference at 24 bits).
- Near plane: F3DZEX2 "NoN" does not clip at the near plane; it clips at w = 0 and clamps each vertex's
  screen z to 0. GX always clips at z_clip = -w, which with k = 1/8 is N64 ndc -7 (a quarter of the
  near distance) instead of -1, so geometry between n/4 and n is drawn; its window depth is below 0 and
  is clamped to 0 (Dolphin; to be checked on hardware: without that clamp those pixels would get a
  wrong depth). Geometry closer than n/4 still disappears. Both clip at the far plane (F3DZEX2 clips
  against it too).
- A triangle whose vertices leave the fitted plane z = a*w + b (non-affine modelview, forced matrix)
  is divided on the CPU (counted as "via CPU" in the stats), as are G_ZS_PRIM triangles (z = the
  primitive depth, clamped to the viewport's depth range) and orthographic projections. The CPU path
  clamps ndc z at -1 like NoN, and drops triangles with a vertex behind the eye (no CPU clipping).
- Primitive depth (G_SETPRIMDEPTH) is the RDP's 15-bit z, which is the RSP's screen z shifted left by 5:
  window depth = z / (32 G_MAXZ), clamped to 1 (0x7FFF is just past G_MAXZ).
- Rectangles are drawn in N64 screen pixels through an orthographic matrix and the full EFB
  viewport; their window depth is the primitive depth with G_ZS_PRIM, else 0.
- Decal bias: GX viewport near/far moved by 2^-16 (256 steps of the 24-bit buffer).

## Combiner and blender (gfx_tev.c)

The N64 color combiner computes (A - B) * C + D per channel, in one or two cycles, separately for
color and alpha. A GX TEV stage computes d + lerp(a, b, c) (op ADD), or d - lerp(a, b, c) (op SUB),
with bias, scale and clamp. One N64 cycle maps to two TEV stages:

```
stage 1: a=0, b=A, c=C, d=D, op ADD  -> A*C + D
stage 2: a=0, b=B, c=C, d=PREV, op SUB -> A*C + D - B*C
```

Use the obvious shortcuts to save stages: B = 0, C = 0, C = 1, A = B, and so on.

- **Inputs:** COMBINED is the previous stage. TEXEL0/1 come from the textures. PRIMITIVE and
  ENVIRONMENT come from konst colors or TEV registers. SHADE is the rasterized vertex color.
  1 and 0 are constants. The alpha forms (TEXEL0_ALPHA, ...) select alpha into color. LOD_FRACTION
  and PRIM_LOD_FRAC become konst colors. NOISE is approximated.
- **Cycle types:** 1-cycle uses only cycle 1's combiner and blender. 2-cycle uses both. COPY mode
  draws the texture directly. FILL mode is handled by gfx_gx.c.
- **Blender** (othermode L render mode). Map it to GX blending:
  - `G_BL_CLR_IN * G_BL_A_IN + G_BL_CLR_MEM * G_BL_1MA` is alpha blending (when FORCE_BL or
    IM_RD/CVG settings make it apply).
  - Fog (`G_BL_CLR_FOG`, `G_BL_A_SHADE`) becomes a final TEV stage, lerp(combined, fog color,
    shade alpha).
  - Opaque surfaces draw without blending.
  - Alpha compare: threshold against blend alpha, or CVG_X_ALPHA, maps to GX_SetAlphaCompare.
- **Z:** Z_CMP/Z_UPD bits give GX_SetZMode. Z_MODE decal sets `decal`.
- **Caching:** cache compiled stage setups by (combineHi, combineLo, cycle type, render mode bits,
  prim kind).

## Textures (gfx_rdp.c, gfx_tex.c)

- **TMEM model:** record each `G_LOADBLOCK`/`G_LOADTILE`/`G_LOADTLUT` as "TMEM words [x, y) came
  from RAM address A (format, size, line stride)". Do not emulate TMEM bytes. When a tile is drawn,
  find the load covering `tile.tmem` and decode from RAM using the tile's size (from
  `G_SETTILESIZE`), line, format and palette. TLUTs are loaded at TMEM 256+ (CI4 palette
  `palette * 16`).
- **Formats:** RGBA16 becomes RGB5A3. RGBA32 becomes RGBA8. IA4, IA8 and IA16 become GX IA4/IA8.
  I4 and I8 become GX I4/I8 (N64 replicates intensity into alpha, as GX does). CI4 and CI8 with an
  RGBA16 TLUT become RGB5A3, and with an IA16 TLUT become IA8 (decoding to direct color is simpler
  than GX TLUTs; either is fine). GX textures are tiled in 4x4/8x4 blocks, so convert on the CPU.
- **Wrap:** GX REPEAT/MIRROR need power-of-two sizes. N64 mask and clamp combinations: build the
  GX texture so that GX wrap and clamp reproduce the N64 result (for example, expand a masked
  region to the clamp size).
- **Cache:** key by source address, format, size, palette address and a cheap content hash of the
  source, with LRU eviction inside a fixed budget (GFX_TEX_CACHE_SIZE, about 2 MiB, from
  gc_mem_alloc). Textures must be 32-byte aligned; flush the CPU data cache after conversion
  (DCFlushRange) and invalidate the GX texture cache (GX_InvalidateTexAll) when entries are reused.

## Frames and video (gfx_gx.c, gfx_task.c)

- The EFB renders at 640x480 (GFX_SCALE 2).
- `gfx_gx_task_end` copies the EFB to an XFB slot keyed by the frame's color image address. Keep
  2-3 slots, reused least recently.
- `gc_gfx_present(fb)` sets that slot as the next framebuffer. The swap therefore happens at the
  N64's own pacing (the game swaps every third retrace at 20 fps).
- FILL mode fill rectangles: a full-screen fill of the frame becomes a clear color. Since the EFB
  is already cleared with it, it can be a GX clear on copy or a quad. A fill of the z-buffer becomes
  a depth clear.
- After gc_halt the console XFB must show again (`gc_ogc_video_show_console`).

As implemented:
- Render targets: the first color image of a task that is not the z image is the frame; a color
  image equal to the z image takes depth fills; any other is off-screen and its draws are skipped
  (first 8 logged). The frame address is normalized to KSEG0 for the XFB slot key.
- EFB clear policy: every EFB copy also clears the EFB, to the color of the last full-screen FILL
  of the frame and to far depth. gfx_gx.c tracks that the EFB is "clean", so the game's full-screen
  fills that follow (same color; z-buffer fill) are skipped. Any other FILL rectangle, or a fill
  after something was drawn, is a quad: color without depth, or depth only (far) with color writes
  off. Frames that do not clear show the clear color, not stale contents.
- XFB slots (3, 2 minimum): `gfx_gx_task_end` picks a slot that is neither on screen
  (VIDEO_GetCurrentFramebuffer: latched at the last retrace) nor about to be (VIDEO_GetNextFramebuffer,
  or the slot last handed to the VI), preferring the one that already holds this N64 framebuffer,
  else the least recently used. With only 2 slots it may have to wait for a retrace. GX_CopyDisp,
  then GX_DrawDone, then the slot is published under the frame's address. The slot table is shared
  with the VI thread under disabled interrupts.
- `gc_gfx_present` (VI thread, from vi.c at the retrace where a swapped buffer becomes current)
  calls `gc_ogc_video_show_frame`. The console stays the display until that first call; from then
  on osViBlack is honoured. After gc_halt, `gc_ogc_video_show_console` wins for good.
- The thread that runs the tasks becomes the current GX thread at each task start, because libogc
  suspends that thread (not the writer) when the FIFO fills up.

## Memory

The GameCube needs 4-5 MiB for the renderer: the GX FIFO (256 KiB), 2-3 XFBs (614,400 bytes each
at 640x480) and the texture cache (about 2 MiB). To make room, the ROM range that the audio driver
reads every frame (0x20700-0x5E06E0, 5.75 MiB) moves from MEM1 to ARAM (bridge_rom.c).

Budget (all from gc_mem_alloc, logged as "mem:" lines, plus a "gfx: GX ready" summary):

| Item | Size |
|---|---|
| GX FIFO | 256 KiB |
| 3 XFBs, 640x480 YUY2 (NTSC; PAL 574 lines is 735 KiB each) | 1,800 KiB |
| Texture cache (gfx_tex.c, halves down to 256 KiB if short) | 2,048 KiB |
| Renderer statics in the image (batch buffer, TEV program cache, texture tables) | ~0.2 MiB |
| **Total** | **~4.1 MiB** |

M2 measurement (Dolphin, US ROM): 3,864 KiB of MEM1 free after boot, 2,655 KiB once the game's
threads exist (their stacks and buffers take ~1.2 MiB after gc_gfx_init). gc_gfx_init therefore
requires 4,104 KiB plus a 1,536 KiB reserve for the game; with less it logs the shortfall and leaves
the renderer off (console display, tasks skipped) instead of starving the game. The renderer is
initialised at the end of gc_ogc_boot, after the ROM preload, so the 5.75 MiB moved to ARAM is what
makes it fit.

## Debugging

`gfx_rsp_stats_frame` logs, every few seconds:
- tasks, vertices, triangles, rectangles, texture conversions and cache hits
- time per task
- each unknown opcode, once
`make -f Makefile.gc GC_RENDERER=0` keeps the M2 text console as the display.

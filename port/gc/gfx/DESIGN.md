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

- **F3DZEX2 and S2DEX2.** The game switches to S2DEX2 (`G_LOAD_UCODE`) for PreRender (motion blur,
  the pause and picto captures), `z_visfbuf.c`, Grandma's story (z_parameter.c) and the wipe5
  transition. `gfx_s2dex.c` interprets it: `G_BG_COPY`/`G_BG_1CYC` are drawn from the whole image
  (`gfx_fb_sync_ram` + `gfx_tex_bind_image` + `gfx_gx_image_rect`) with the microcode's clipping,
  frame limit, flips and wraps; sprites become RDP texture rectangles. MM never draws pre-rendered
  room backgrounds (`z_room.c` has `isFixedCamera = false`) and never calls `Jpeg_Decode`;
  `port/gc/game/njpeg_cpu.c` still runs the RSP JPEG task on the CPU.
- **Render targets.** The first `G_SETCIMG` of a task is the frame. A FILL into the z image clears
  depth (the N64 clears the z-buffer by filling it as a color image with `0xFFFC`). Any other color
  image, including colors drawn into the z-buffer's memory, is an off-screen pass drawn into the EFB
  and copied to RAM (see "Framebuffer effects").
- **Framebuffer RAM on demand.** N64 images the renderer drew are written to RAM when something reads
  them (textures, S2DEX2 backgrounds, the CPU after the task), and the z-buffer's RAM gets the frame's
  depth for the game's pixel reads (see "Framebuffer effects").

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
- Render targets: the first color image of a task that is not the z image is the frame; FILL-mode
  rectangles into the z image clear depth; any other color image (other draws into the z image too)
  is an off-screen pass, drawn as described in "Framebuffer effects". The frame address is normalized
  to KSEG0 for the XFB slot key.
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

## Framebuffer effects (gfx_fb.c)

On the N64 every color image lives in RAM: effects copy the frame to another buffer, draw it back
blended or scaled, load it as a texture, or filter it on the CPU. On the GameCube the frame lives in
the EFB at 2x and the renderer never writes N64 framebuffer RAM by itself. What reads or draws N64
images in MM:

| Effect | Code | Needs |
|---|---|---|
| Motion blur | z_play.c Play_DrawMotionBlur, PreRender.c | S2DEX2 copy of the frame to gWorkBuffer (off-screen), next frame blends it back |
| Pause, picto box, transition tile captures | z_play.c, PreRender.c | frame copied to the z-buffer's memory (off-screen), coverage to an I8 image; CPU filters and the picto I8 conversion read them after the task |
| VisMono | z_vismono.c | the frame loaded as CI8 textures while drawing into it (F3DZEX2) |
| VisFbuf (screen shrink, wipe4), wipe5 | z_visfbuf.c, ovl_fbdemo_wipe5 | frame copied off-screen, then drawn back scaled/blended, or drawn over itself |
| Lens of Truth | z_actor.c | the z-buffer copied as an RGBA16 image and back (off-screen) |
| Sun lens flare, light glows | z_kankyo.c, z_lights.c | SysCfb_GetZBufferPixel: z-buffer RAM after the task |

Design:
- **Off-screen color images are drawn in the EFB at 1x**, in its top-left 320x240 (the canvas). The
  EFB (640x528) has no room beside the 640x480 frame, so when a pass starts (gfx_gx.c's target
  selection, before the draw binds textures) the frame's pixels there are copied to an RGBA8 texture
  and its depth to a Z24X8 texture (GPU copies, no CPU work). The pass draws with scale 1 (viewport,
  scissor and screen rectangles follow `sScale` in gfx_gx.c). When the color image switches back to
  the frame, or at task end, the canvas goes to RAM and the frame's color and depth are drawn back
  (a Z texture with GX_ZT_REPLACE restores the depth exactly). Drawing at 1x is the N64's own
  resolution for these images, and the copy back to RAM is 1:1.
- **Only pixels that were drawn go back to RAM.** The canvas is not loaded from RAM when a pass
  starts. While every draw is an opaque rectangle (no IM_RD, no alpha compare: BG copies, coverage
  strips) the dirty rectangle stays exactly what was drawn (a draw that would make it inexact writes
  the previous one out first). The first draw that reads memory (blending, alpha compare, triangles)
  writes what was drawn so far, then loads the whole image from RAM into the canvas.
- **Frame readback is lazy.** gfx_gx.c records which N64 rows of the frame were drawn since its RAM
  was last written. `gfx_fb_sync_ram(addr, bytes)` writes them (EFB copy at half size with the 2x2
  box filter to RGB565, converted to RGBA5551 with the alpha bit set) only if the range overlaps them.
  It is called by gfx_gx.c before binding tiles (the source is estimated from the last G_SETTIMG and
  the tile's rows; VisMono's 80 strips each read rows not drawn yet, so the frame is read once), by
  gfx_s2dex.c before binding a background image, and at the start of every off-screen pass (passes
  nearly always copy the frame, and its pixels under the canvas are about to be replaced). The
  CPU consumers (PreRender filters, the picto box) read images that off-screen passes wrote; those
  are in RAM when the task ends. After writing RAM, gfx_tex_ram_written() makes the texture cache
  hash that memory again.
- **Depth for the game's pixel reads.** At the end of a task that cleared the z-buffer and drew no
  colors into its memory afterwards (pause/picto/transition captures and Lens of Truth use it as a
  color buffer), the EFB depth is copied 1:1 a quarter at a time (a box-filtered depth copy would
  average the 24-bit values per byte channel) and converted to the N64 format (18-bit z = screen z
  << 8, compressed to a 3-bit exponent of leading ones and an 11-bit mantissa; never drawn pixels give
  0xFFFC, the clear value the lens flare test looks for). One value per 4x4 pixels: the readers test
  single pixels loosely. `-DGFX_FB_DEPTH=0` turns it off.
- **VisMono is drawn natively.** VisMono_DesaturateDList loads the frame 3 rows at a time as CI8
  through an IA16 palette and draws it back with `dsdx = 2`: at 2x the EFB samples each N64 pixel at
  two different bytes (vertical stripes), and every strip would convert two textures (about 44 ms per
  frame in Dolphin). gfx_gx.c recognizes its rectangles (combiner, IA16 TLUT, CI8 tiles, texture
  image = the frame row being drawn) and draws them from `gfx_fb_frame_texture()` (an EFB copy of
  the frame at N64 size, GPU only) with four TEV stages: the channels summed with weights 2/7, 4/7,
  1/7 through the texture swap tables (the palette's I + A), then lerp(ENV, PRIM), blended by prim
  alpha. It needs no readback, so it also works where EFB copies do not reach RAM.
- **EFB copies must reach RAM.** gfx_fb_init probes it (an 8x8 copy cleared to a known color, read
  back by the CPU). On the GameCube they always do; Dolphin's "Store EFB Copies to Texture Only"
  (`Graphics.Hacks.EFBToTextureEnable`, on by default) keeps them on the host GPU, so the probe fails
  and everything but VisMono stays off (off-screen draws skipped, no readback, no depth), as before
  gfx_fb.c. Test with `-ExtraConfig 'Graphics.Hacks.EFBToTextureEnable=False'`, and preferably
  `Graphics.Settings.SafeTextureCacheColorSamples=0`: Dolphin's default texture cache hashes 128
  samples, misses small changes when the same memory is rewritten (as gfx_tex.c's arena is), and
  showed about 20 stale texels in half of the frames of the copy test.
- Everything gfx_fb.c draws uses GX_TEXMAP6/7 and GX_VTXFMT1, then calls `gfx_gx_state_lost()`;
  texture copies use center sampling and the 1-line vertical filter, and the video mode's filter is
  set again for the display copy.
- Not emulated: coverage (the EFB has no coverage or destination alpha; G_RM_VISCVG draws full
  coverage, so PreRender's anti-alias and divot filters find nothing to do and the pause background is
  the unfiltered frame); the z-buffer as an RGBA16 color image (Lens of Truth's copy of it and back
  are color copies of RAM; the depth mask it draws into the EFB stays until the end of the frame);
  hi-res (576x454) frames (the Bombers' Notebook) are not read back.

Memory: 750 KiB from the MEM1 arena (transfer buffer 150 KiB, frame color and depth under the canvas
300 KiB each, also used for the VisMono frame copy and the depth quarters), allocated only if 1.5 MiB
stay free for the game; otherwise off-screen draws are skipped as before.

Measured (Dolphin 2609, time in gfx_fb.c per task from the timebase, GX waits included but under
50 us; real hardware will differ, cache misses on the 150-300 KiB buffers most of all):
- Depth write, every frame that clears the z-buffer: 0.67 ms.
- Motion blur (title intro, SPOT00, alpha 180 every frame): one frame readback, one off-screen pass
  and its copy to RAM, depth: 2.0-3.7 ms. The rest of the effect's cost is in gfx_tex.c, which
  converts the two 320x240 RGBA16 backgrounds (frame and work buffer) every frame since both change;
  gfx_rsp's task time goes from 6.5 to 11.3 ms there. The game stays at 20 fps.
- VisMono over the attract scenes (test build): 80 rectangles per frame, 0.5 ms of gfx_fb.c (the
  depth write) and 9-13 ms per task, against 44 ms through the texture path.
- A 320x240 conversion (frame readback or off-screen copy) is about 0.9 ms; conversions work on
  4-texel tile rows with 32-bit operations.

Tests: `GFX_FB_TEST` (gfx_task.c, compile-time, off by default) appends the game's own effect display
lists to every frame (VisMono, motion blur with texture rectangles in place of S2DEX2, a VisFbuf-style
shrink, a coverage-style I8 image, a pause-style capture into the z-buffer's memory) and checks the
RAM results: the copy of the frame equals the frame read back before the pass, the frame after the
pass equals it too (800 of 800 frames, with the safe texture cache), the I8 image equals the frame's
high bytes (400 of 400), and the z-buffer capture equals the frame and is still there at the next
task, so no depth was written over it (bit 16: 600 of 600). Build it in its own build directory
(make does not track flags):
`make -f Makefile.gc BUILD_DIR=build/gc-fbtest GFX_CFLAGS='$(OGC_CFLAGS) -Iport/gc/gfx -Iport/gc/ogc
-Ibuild/gc-fbtest/generated -DGFX_FB_TEST=2'`.

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
| Framebuffer effects (gfx_fb.c; only if 1.5 MiB stay free) | 750 KiB |
| **Total** | **~4.8 MiB** |

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

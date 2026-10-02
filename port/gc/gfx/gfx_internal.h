/**
 * Internal contract of the N64 display list renderer (port/gc/gfx).
 *
 * The renderer executes the game's F3DZEX2 graphics tasks (gspF3DZEX2_NoN_PosLight_fifo) on the
 * GameCube. It walks each display list as the RSP microcode would: it transforms, lights and fogs
 * vertices on the CPU, tracks RDP state, and draws through GX into the EFB, which is copied to an
 * XFB for display when the game swaps to the matching N64 framebuffer. See port/gc/gfx/DESIGN.md.
 *
 *   gfx_rsp.c   display list interpreter: RSP commands, matrices, vertex pipeline, triangles, stats
 *   gfx_rdp.c   RDP state commands, texture/fill rectangles
 *   gfx_tex.c   TMEM load tracking, N64 -> GX texture conversion, texture cache
 *   gfx_tev.c   color combiner, blender, alpha compare, z mode -> GX TEV and pixel engine state
 *   gfx_gx.c    GX setup, EFB/XFB, viewport, scissor, projection, drawing, presenting frames
 *   gfx_task.c  entry points declared in gc_bridge.h (gc_gfx_*)
 *
 * All of port/gc/gfx is compiled with libogc headers (OGC_CFLAGS), never the decomp's headers.
 * GBI constants come from gfx_gbi.h; N64 memory structures (Mtx, Vtx, Light, Viewport) are parsed
 * from raw big-endian bytes, which is the GameCube's native byte order.
 *
 * Coordinates: N64 screen space is 320x240 (subpixel 10.2 fixed point in RDP commands). The EFB is
 * rendered at GFX_SCALE times that resolution.
 *
 * Threading: every gfx_* function except gfx_gx_present() runs on the thread that called
 * gc_gfx_run_task() (the game's Sched thread). gfx_gx_present() runs on the VI service thread.
 */
#ifndef GFX_INTERNAL_H
#define GFX_INTERNAL_H

#include <gccore.h>
#include <stdbool.h>
#include <stdint.h>
#include "gfx_gbi.h"
#include "gc_bridge.h"

#define GFX_N64_WIDTH 320
#define GFX_N64_HEIGHT 240
#define GFX_SCALE 2
#define GFX_EFB_WIDTH (GFX_N64_WIDTH * GFX_SCALE)
#define GFX_EFB_HEIGHT (GFX_N64_HEIGHT * GFX_SCALE)

/* ================================================================================================ */
/* Addresses (gfx_rsp.c)                                                                            */
/* ================================================================================================ */

/** RSP segment table (G_MW_SEGMENT). Entries are physical addresses, as the game writes them. */
extern uint32_t gGfxSegments[16];

/**
 * Convert an address found in a display list to a CPU pointer:
 *   0                      -> NULL
 *   bit 31 set (KSEG0/1)   -> 0x80000000 | (addr & 0x1FFFFFFF) (GameCube RAM is not limited to 16 MB)
 *   otherwise (segmented)  -> 0x80000000 | (gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF))
 */
void* gfx_addr(uint32_t addr);

/* ================================================================================================ */
/* Processed vertices: output of the RSP vertex stage (gfx_rsp.c), input of gfx_gx.c                */
/* ================================================================================================ */

#define GFX_CLIP_NEG_X 0x01
#define GFX_CLIP_POS_X 0x02
#define GFX_CLIP_NEG_Y 0x04
#define GFX_CLIP_POS_Y 0x08
#define GFX_CLIP_NEG_W 0x10 /* behind the eye (w <= 0) */
#define GFX_CLIP_FAR 0x20

typedef struct {
    float x, y, z, w; /* clip space: position * modelview * projection (N64/OpenGL convention, z in [-w, w]) */
    float s, t;       /* texture coordinates in texels (the RSP's s10.5 output / 32), after G_TEXTURE scale
                         and texgen; tile offset, shift and size are applied at draw time (gfx_tex_uv) */
    uint8_t r, g, b, a; /* shade: lit color or vertex color; a = vertex alpha, or the fog factor with G_FOG */
    uint8_t clip;       /* GFX_CLIP_* */
    uint8_t pad[3];
} GfxVtx;

/* ================================================================================================ */
/* RSP state shared with other modules (gfx_rsp.c owns it)                                          */
/* ================================================================================================ */

typedef struct {
    uint32_t geometryMode; /* G_ZBUFFER, G_SHADE, G_CULL_*, G_FOG, G_LIGHTING, G_TEXTURE_GEN*, G_SHADING_SMOOTH... */
    /* G_TEXTURE */
    uint8_t textureOn;    /* G_ON / G_OFF */
    uint8_t textureTile;  /* tile for TEXEL0 (TEXEL1 uses textureTile + 1) */
    uint8_t textureLevels;
    float textureScaleS, textureScaleT; /* scale / 65536 */
    /* G_MW_FOG */
    int16_t fogMultiplier, fogOffset;
    /* G_MV_VIEWPORT, as written by the game (scale and translation, 2 fractional bits) */
    int16_t viewportScale[4];
    int16_t viewportTrans[4];
} GfxRspState;

extern GfxRspState gGfxRsp;

void gfx_rsp_reset(void);
/** Run a display list until its G_ENDDL (following G_DL calls and branches). */
void gfx_rsp_run(uint32_t dlAddr);
/** Log statistics every few seconds (called by gfx_task.c after each task). */
void gfx_rsp_stats_frame(void);

/* ================================================================================================ */
/* RDP state (gfx_rdp.c owns it; gfx_tex.c, gfx_tev.c and gfx_gx.c read it)                         */
/* ================================================================================================ */

typedef struct {
    uint8_t fmt, siz;     /* G_IM_FMT_*, G_IM_SIZ_* */
    uint16_t line;        /* row stride in 64-bit TMEM words */
    uint16_t tmem;        /* TMEM address in 64-bit words */
    uint8_t palette;      /* CI4 palette index */
    uint8_t cms, cmt;     /* G_TX_MIRROR | G_TX_CLAMP */
    uint8_t masks, maskt; /* wrap mask (log2 of size), 0 = none */
    uint8_t shifts, shiftt;
    uint16_t uls, ult, lrs, lrt; /* G_SETTILESIZE, 10.2 fixed point texels */
} GfxTile;

/* Dirty bits: set by gfx_rdp.c/gfx_rsp.c when the corresponding state changes; gfx_gx.c re-applies
 * GX state before the next draw and clears them. */
#define GFX_DIRTY_COMBINE 0x0001  /* G_SETCOMBINE */
#define GFX_DIRTY_OTHERMODE 0x0002 /* othermode H or L (cycle type, render mode, z, alpha compare, filter) */
#define GFX_DIRTY_COLORS 0x0004   /* prim, env, fog, blend, fill colors, prim LOD */
#define GFX_DIRTY_TEXTURES 0x0008 /* tiles, tile sizes, loads, G_TEXTURE */
#define GFX_DIRTY_GEOMETRY 0x0010 /* geometry mode */
#define GFX_DIRTY_SCISSOR 0x0020
#define GFX_DIRTY_VIEWPORT 0x0040
#define GFX_DIRTY_ALL 0xFFFF

typedef struct {
    uint32_t otherModeH; /* G_SETOTHERMODE_H word (cycle type, texture filter, TLUT, ...) */
    uint32_t otherModeL; /* G_SETOTHERMODE_L word (render mode, z source, alpha compare) */
    uint32_t combineHi;  /* G_SETCOMBINE w0 & 0x00FFFFFF */
    uint32_t combineLo;  /* G_SETCOMBINE w1 */
    uint32_t primColor, envColor, fogColor, blendColor; /* 0xRRGGBBAA */
    uint32_t fillColor;  /* raw 32-bit fill word (two RGBA5551 pixels, or a z value when filling the z-buffer) */
    uint8_t primLodMin, primLodFrac;
    uint16_t primDepthZ, primDepthDZ;
    GfxTile tiles[8];
    /* G_SETTIMG */
    uint32_t texImageAddr;
    uint8_t texImageFmt, texImageSiz;
    uint16_t texImageWidth;
    /* G_SETCIMG / G_SETZIMG (addresses as written by the game) */
    uint32_t colorImageAddr;
    uint8_t colorImageFmt, colorImageSiz;
    uint16_t colorImageWidth;
    uint32_t zImageAddr;
    /* G_SETSCISSOR, 10.2 fixed point */
    uint16_t scissorUlx, scissorUly, scissorLrx, scissorLry;
    uint8_t scissorMode;
    uint32_t dirty; /* GFX_DIRTY_* */
} GfxRdpState;

extern GfxRdpState gGfxRdp;

void gfx_rdp_reset(void);
/** Execute an RDP command forwarded by the RSP (opcodes G_SETCIMG..G_RDPLOADSYNC, G_SETOTHERMODE_*,
 *  G_SETCOMBINE, colors, tiles, loads, scissor, fill rectangles; not G_TEXRECT/G_TEXRECTFLIP). */
void gfx_rdp_command(uint32_t w0, uint32_t w1);
/** G_TEXRECT/G_TEXRECTFLIP: w0/w1 plus the words of the G_RDPHALF_1 and G_RDPHALF_2 commands that follow. */
void gfx_rdp_texrect(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip);

/* ================================================================================================ */
/* Textures (gfx_tex.c)                                                                             */
/* ================================================================================================ */

/** Texture as bound for a draw: everything gfx_gx.c needs to turn texel coordinates into GX UVs. */
typedef struct {
    bool valid;
    uint16_t width, height; /* size of the GX texture in texels */
    float sOffset, tOffset; /* subtract from (s * shiftScale) before dividing by width/height */
    float sShiftScale, tShiftScale; /* 2^-shift (shift 11..15 = 2^(16-shift)) */
    bool linear; /* bilinear filtering (othermode H texture filter) */
} GfxTexBinding;

void gfx_tex_init(void);
void gfx_tex_reset(void); /* per task */
/** Record a G_LOADBLOCK / G_LOADTILE / G_LOADTLUT into the TMEM model (called by gfx_rdp.c). */
void gfx_tex_load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt);
void gfx_tex_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
void gfx_tex_load_tlut(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt);
/** Convert (or find in the cache) the texture for `tile` and load it into GX texture map `texMap`
 *  (GX_TEXMAP0/1). Returns false if the tile cannot be resolved (nothing loaded). */
bool gfx_tex_bind(int tile, int texMap, GfxTexBinding* out);
/** GX UV for a texel-space coordinate under a binding. */
static inline void gfx_tex_uv(const GfxTexBinding* b, float s, float t, float* u, float* v) {
    *u = (s * b->sShiftScale - b->sOffset) / (float)b->width;
    *v = (t * b->tShiftScale - b->tOffset) / (float)b->height;
}
/** Drop cached textures whose source memory the game rewrote (called at task start). */
void gfx_tex_frame(void);

/** Texture cache counters since boot (gfx_rsp_stats_frame logs them). */
typedef struct {
    uint32_t binds;      /* gfx_tex_bind calls */
    uint32_t hits;       /* binds served by an already converted texture */
    uint32_t misses;     /* binds that converted a new texture */
    uint32_t reconverts; /* cached textures converted again because the game rewrote their source */
    uint32_t evictions;  /* textures dropped to make room */
    uint32_t syncs;      /* GX_DrawDone waits: evicting a texture already drawn in the same task */
    uint32_t failures;   /* binds that returned false (nothing loaded, too large, no memory) */
    uint32_t slow;       /* binds that replayed TMEM because the tile is not rows of one load in RAM */
    uint32_t entries;    /* textures in the cache */
    uint32_t bytesUsed, bytesTotal; /* texture memory */
} GfxTexStats;
void gfx_tex_get_stats(GfxTexStats* out);

/**
 * Bind a whole image straight from RAM, bypassing the TMEM model (S2DEX2 backgrounds and object
 * sprites, framebuffer-sourced textures). `addr` is a CPU pointer; fmt/siz are G_IM_FMT_* /
 * G_IM_SIZ_*; `stride` is the row stride in texels; `tlut` is the palette in RAM for CI formats
 * (RGBA16 entries, or IA16 when tlutIA), NULL otherwise. The texture uses clamp wrapping; `linear`
 * selects bilinear filtering. Cached like other textures (content-hashed once per task).
 * The binding's UVs map texel (s, t) of the image as gfx_tex_uv does (offsets 0, shift scale 1).
 */
bool gfx_tex_bind_image(const void* addr, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height, uint16_t stride,
                        const void* tlut, bool tlutIA, bool linear, int texMap, GfxTexBinding* out);

/**
 * N64 RAM [addr, addr + bytes) (KSEG0 or physical address) was rewritten in the middle of a task, by the renderer
 * itself (gfx_fb.c writing EFB pixels back): cached textures and palettes read from that range are hashed again at
 * their next bind (contents are otherwise checked once per task). Safe while draws are queued: nothing is freed.
 */
void gfx_tex_ram_written(uint32_t addr, uint32_t bytes);

/* ================================================================================================ */
/* Combiner / blender (gfx_tev.c)                                                                   */
/* ================================================================================================ */

typedef enum {
    GFX_PRIM_TRIANGLE, /* shade comes from vertex colors */
    GFX_PRIM_TEXRECT,  /* no shade coefficients on the RDP */
    GFX_PRIM_FILLRECT  /* 1/2-cycle fill rectangle (FILL mode is handled by gfx_gx.c directly) */
} GfxPrimKind;

typedef struct {
    bool usesTexel0, usesTexel1; /* textures the current combiner reads (TEXEL1 = tile + 1) */
    bool usesShade;              /* vertex colors needed */
    bool decal;                  /* z mode decal: gfx_gx.c biases depth toward the viewer */
    bool depthTest, depthWrite;
} GfxTevInfo;

void gfx_tev_init(void);
/** Program GX TEV stages, konst colors / TEV registers, blend mode, alpha compare, z mode and color/alpha
 *  update from gGfxRdp (combine, othermode, colors) and gGfxRsp.geometryMode for a primitive kind.
 *  Uses GX_COLOR0A0 as the rasterized shade and GX_TEXMAP0/GX_TEXCOORD0 (TEXEL0), GX_TEXMAP1/GX_TEXCOORD1
 *  (TEXEL1). Does not touch vertex formats, texgens, channels or texture objects (gfx_gx.c owns those). */
void gfx_tev_apply(GfxPrimKind kind, GfxTevInfo* out);
/** gfx_tev_apply() skips uploading TEV stages and pixel engine state that GX already holds. Call this after
 *  changing TEV stages, TEV colors, blend mode, alpha compare, z mode or color/alpha update outside gfx_tev.c
 *  (FILL mode rectangles, EFB copies), so that the next gfx_tev_apply() uploads everything again. */
void gfx_tev_invalidate(void);

/* ================================================================================================ */
/* GX backend (gfx_gx.c)                                                                            */
/* ================================================================================================ */

/** Once at boot, after the console video init: FIFO, EFB/XFB, vertex format. Takes over the display
 *  unless the renderer is disabled. */
void gfx_gx_init(void);
void gfx_gx_task_begin(void);
/** Flush, then copy the EFB to the XFB associated with the frame's color image. */
void gfx_gx_task_end(void);
/** Show the XFB that holds the frame rendered into N64 framebuffer `n64Framebuffer` (VI thread). */
void gfx_gx_present(const void* n64Framebuffer);

/** Projection for the vertices that follow (gfx_rsp.c calls this whenever the projection matrix changes).
 *  m is row-major in the N64 sense: clip = v * m (row vector times matrix). */
void gfx_gx_set_projection(const float m[4][4]);

/** Draw one triangle with the current RSP/RDP state (culling has already been done by gfx_rsp.c). */
void gfx_gx_triangle(const GfxVtx* v0, const GfxVtx* v1, const GfxVtx* v2);

/** Rectangle in N64 screen coordinates (pixels, float). For texture rectangles, s/t are the texel
 *  coordinates at (ulx, uly) and dsdx/dtdy the steps per pixel (already divided by 4 in copy mode);
 *  flip swaps the s and t axes (G_TEXRECTFLIP). */
void gfx_gx_texrect(float ulx, float uly, float lrx, float lry, int tile, float s, float t, float dsdx,
                    float dtdy, bool flip);
/** Fill rectangle (G_FILLRECT): FILL cycle mode uses gGfxRdp.fillColor (or clears depth when the color
 *  image is the z-buffer); 1/2-cycle mode draws through the combiner. */
void gfx_gx_fillrect(float ulx, float uly, float lrx, float lry);

/** Flush pending draws (state is about to change). */
void gfx_gx_flush(void);
/** True once gfx_gx_init() has set up GX, the FIFO and the XFBs (false: not enough memory or no video mode;
 *  gfx_task.c then leaves the renderer disabled). */
bool gfx_gx_ready(void);

/** GX state was changed outside gfx_gx.c/gfx_tev.c (gfx_fb.c's copies and quads: vertex format, channels, TEV,
 *  projection, viewport, scissor, blend/z modes): re-apply everything before the next draw. */
void gfx_gx_state_lost(void);

/**
 * Texture rectangle whose TEXEL0 is an image the caller bound to GX_TEXMAP0 with gfx_tex_bind_image() (`b`), for
 * S2DEX2 backgrounds and object sprites. Otherwise like gfx_gx_texrect: N64 screen coordinates, (s, t) the texel
 * coordinates at (ulx, uly), dsdx/dtdy per pixel, the combiner as for GFX_PRIM_TEXRECT, the current color image
 * as the render target. TEXEL1 reads opaque white. Selecting the render target never touches GX_TEXMAP0/1, so
 * binding first is safe; call gfx_fb_sync_ram() for the image's pixels before gfx_tex_bind_image().
 */
void gfx_gx_image_rect(float ulx, float uly, float lrx, float lry, const GfxTexBinding* b, float s, float t,
                       float dsdx, float dtdy);

/* ================================================================================================ */
/* Framebuffer effects (gfx_fb.c): N64 color images in RAM versus the EFB                          */
/* ================================================================================================ */

/** What gfx_gx.c and gfx_fb.c share about the frame (gfx_fb.c owns it). */
typedef struct {
    uint32_t frameKey;         /* the frame's color image (KSEG0 address), 0 until the task's first draw into it */
    uint16_t frameWidth;       /* its pixels per row */
    int16_t dirtyY0, dirtyY1;  /* N64 rows [y0, y1) drawn in the EFB since the frame's RAM was last written */
    bool canvasDirty;          /* the off-screen image being drawn has EFB pixels that are not in RAM yet */
    uint32_t frameGen;         /* counts draws into the frame (copies of it made before a draw are out of date) */
} GfxFbState;

extern GfxFbState gGfxFb;

/** A draw into the frame may have changed N64 rows [y0, y1). */
static inline void gfx_fb_frame_drawn(int y0, int y1) {
    if (y0 < gGfxFb.dirtyY0) {
        gGfxFb.dirtyY0 = y0;
    }
    if (y1 > gGfxFb.dirtyY1) {
        gGfxFb.dirtyY1 = y1;
    }
    gGfxFb.frameGen++;
}

/** True if some N64 image has newer pixels in the EFB than in RAM (gfx_fb_sync_ram would have work to do). */
static inline bool gfx_fb_pending(void) {
    return gGfxFb.dirtyY0 < gGfxFb.dirtyY1 || gGfxFb.canvasDirty;
}

/** Once at boot, after gfx_gx_init(): transfer and save buffers. Without them, off-screen color images are not
 *  drawn and framebuffers are not read back (as before gfx_fb.c). */
void gfx_fb_init(void);
bool gfx_fb_ready(void);
/** Task start: nothing drawn yet (the frame's RAM keeps what it had, as on the N64). */
void gfx_fb_task_begin(void);
/** The task's frame (gfx_gx.c, at its first draw). */
void gfx_fb_set_frame(uint32_t key, uint16_t width);

/**
 * N64 RAM [addr, addr + bytes) is about to be read, as a texture or by the CPU: if the renderer holds newer pixels
 * for it in the EFB (the frame, or the off-screen image being drawn), write them to RAM first (an EFB copy, a GX
 * sync and a conversion on the CPU; textures cached from that memory are checked again). Batched draws are
 * submitted first (gfx_gx_flush), so a texture bound right after cannot reach draws queued before. Cheap when
 * nothing is pending. Anything that reads a framebuffer address bypassing the TMEM model must call it, such as
 * callers of gfx_tex_bind_image().
 */
void gfx_fb_sync_ram(const void* addr, uint32_t bytes);

/** Binds an image from RAM as gfx_tex_bind_image() does (gfx_fb_bind_image's way back to RAM; passed in, so that
 *  gfx_fb.c does not depend on gfx_tex.c) */
typedef bool (*GfxBindImageFn)(const void* addr, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height,
                               uint16_t stride, const void* tlut, bool tlutIA, bool linear, int texMap,
                               GfxTexBinding* out);

/**
 * Bind an RGBA16 image the renderer holds on the GPU to `texMap`, instead of gfx_fb_sync_ram() + gfx_tex_bind_image():
 * the frame (`addr` is the task's frame and every row of its RAM is, or would be after gfx_fb_sync_ram, what the EFB
 * holds: an EFB copy at N64 size), or the off-screen image last written to RAM whole (the EFB copy that wrote it, while
 * its RAM still matches: checked once per task). The texture is exactly what gfx_tex_bind_image would make after
 * gfx_fb_sync_ram (RGB5A3 texels of RGBA5551 pixels with the alpha bit set, `width` x `height`, clamp, `linear`
 * filtering) and so is the binding, without the RAM round trip, hashing or texture cache entry; the frame's RAM stays
 * pending. Images of GFX_N64_WIDTH x GFX_N64_HEIGHT pixels (`stride` the same width) only. False: bind it from RAM.
 * Batched draws are submitted first (gfx_gx_flush).
 * The texture lives in memory gfx_fb.c reuses for its own copies. Draws queued before a reuse still read it (EFB copies
 * are executed after them; CPU writes wait for them), but gfx_gx_image_rect() selects its render target and prepares
 * the canvas (which can write or load an off-screen image) after the caller bound the texture and before it queues
 * the draw. So until gfx_fb_image_done(), a reuse first moves the binding to RAM: the image's pixels go to RAM if it is
 * the frame, and `fromRam` (gfx_tex_bind_image) binds it to `texMap` from there, the same texels and binding.
 */
bool gfx_fb_bind_image(const void* addr, uint16_t width, uint16_t height, uint16_t stride, bool linear, int texMap,
                       GfxBindImageFn fromRam, GfxTexBinding* out);
/** Every draw of the image gfx_fb_bind_image() served last has been issued (after the background's last
 *  gfx_gx_image_rect): its memory may be reused without moving the binding to RAM. */
void gfx_fb_image_done(void);

/**
 * Off-screen pass (gfx_gx.c): from now on draws go to the N64 color image `key` (fmt/siz, `width` pixels per row)
 * at 1x in the EFB's top-left corner (the canvas). The first one saves the frame's pixels there (color and depth)
 * and writes the frame's pending rows to RAM; switching from another off-screen image writes that one to RAM.
 * Returns false for images it cannot draw (format, width over 320, no buffers): their draws are skipped.
 */
bool gfx_fb_canvas_begin(uint32_t key, uint8_t fmt, uint8_t siz, uint16_t width);
/** Before a draw into the canvas covering canvas pixels [x0, x1) x [y0, y1): `opaque` if it sets every one of
 *  them without reading the image (a rectangle without memory reads or alpha compare); otherwise the canvas is
 *  loaded from RAM first. May change GX state (calls gfx_gx_state_lost). */
void gfx_fb_canvas_draw(int x0, int y0, int x1, int y1, bool opaque);
/** End of the off-screen pass: the image goes to RAM, the frame's pixels and depth come back. */
void gfx_fb_canvas_end(void);
bool gfx_fb_canvas_active(void);

/** A GX texture of the frame as the EFB holds it now, at N64 size (EFB copy, 2x2 box filter, RGB565): GPU work
 *  only, nothing goes to RAM. Valid until the next call or off-screen pass. False without buffers or during an
 *  off-screen pass. (VisMono, gfx_gx.c) */
bool gfx_fb_frame_texture(GXTexObj* out);

/** Task end, before the EFB is copied to the XFB and cleared. `writeDepth`: the task cleared the z-buffer and
 *  nothing else was written to it since, so its RAM gets the frame's depth (N64 format) for the game's
 *  SysCfb_GetZBufferPixel readers. */
void gfx_fb_task_end(const void* zImage, bool writeDepth);

typedef struct {
    uint32_t readbacks, readbackRows; /* frame rows written to RAM */
    uint32_t passes;                  /* off-screen passes (canvas begin from the frame) */
    uint32_t copyOuts, uploads;       /* canvas -> RAM, RAM -> canvas */
    uint32_t depthWrites;             /* z-buffer written to RAM */
    uint64_t ticks;                   /* time in gfx_fb.c... */
    uint64_t waitTicks;               /* ...of which waiting for GX to finish (draws queued before included) */
} GfxFbStats;
/** Counters since the last call (gfx_gx.c logs them with its statistics), then reset. */
void gfx_fb_take_stats(GfxFbStats* out);

/* ================================================================================================ */
/* S2DEX2 (gfx_s2dex.c): the 2D microcode, run by gfx_rsp.c while it is loaded                      */
/* ================================================================================================ */

/** Per task (from gfx_rsp_reset). What gfx_s2dex_note_image / _note_tlut recorded is kept across tasks. */
void gfx_s2dex_reset(void);
/** G_LOAD_UCODE of S2DEX2: its DMEM data comes back (object render mode, status words, 2D matrix). */
void gfx_s2dex_load(void);
/** Execute an S2DEX2-only command (G_BG_*, G_OBJ_*). Backgrounds are drawn from the whole image (gfx_fb_bind_image
 *  for the frame and captures, else gfx_fb_sync_ram + gfx_tex_bind_image from RAM; gfx_gx_image_rect); sprites as the
 *  microcode draws them, with RDP commands
 *  (render tile, TMEM loads, texture rectangles) through gfx_rdp_command() / gfx_rdp_texrect(). Returns false if
 *  the opcode is not one of them (G_SELECT_DL and the commands shared with F3DEX2 are gfx_rsp.c's). */
bool gfx_s2dex_command(uint32_t w0, uint32_t w1);
/** G_SELECT_DL with the words of the G_RDPHALF_0 before it: true if the status word changes, in which case the
 *  display list *dl must be called (*push) or branched to. */
bool gfx_s2dex_select_dl(uint32_t half0W0, uint32_t half0W1, uint32_t w0, uint32_t w1, uint32_t* dl, bool* push);
/** G_MOVEWORD G_MW_GENSTAT (gSPSetStatus): `ofs` is the status id (0, 4, 8, 12). */
void gfx_s2dex_set_status(uint32_t ofs, uint32_t value);
/** A G_SETCIMG / G_SETZIMG went to the RDP (any microcode): remember the image as an N64 render target. Without
 *  gfx_fb.c (gfx_fb_ready() false) the RAM of render targets never holds what was drawn there, so backgrounds read
 *  from one (motion blur, pause background, transitions) are skipped instead of drawing stale memory. */
void gfx_s2dex_note_image(uint32_t w0, uint32_t w1);
/** A G_LOADTLUT went to the RDP (any microcode; gGfxRdp already updated): where the TLUT entries came from, for the
 *  palettes of CI backgrounds (gfx_tex_bind_image takes the palette's RAM address). */
void gfx_s2dex_note_tlut(uint32_t w0, uint32_t w1);

/** S2DEX2 counters since boot (gfx_rsp_stats_frame logs the activity of each interval). */
typedef struct {
    uint32_t bgs;       /* G_BG_COPY / G_BG_1CYC drawn */
    uint32_t rects;     /* image rectangles they took (wrapped images take several) */
    uint32_t targetBgs; /* backgrounds read from an N64 render target (synced through gfx_fb.c, or skipped) */
    uint32_t skipped;   /* backgrounds not drawn: render target without gfx_fb.c, bad image, bind failure */
    uint32_t objs;      /* sprites drawn (G_OBJ_RECTANGLE, _R, G_OBJ_SPRITE and the G_OBJ_LDTX_* forms) */
} GfxS2dexStats;
void gfx_s2dex_get_stats(GfxS2dexStats* out);

#endif

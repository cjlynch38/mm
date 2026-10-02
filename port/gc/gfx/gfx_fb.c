/**
 * Framebuffer effects of the N64 renderer (DESIGN.md, "Framebuffer effects"): N64 color images in RAM versus what
 * GX draws in the EFB.
 *
 *  - The frame is drawn in the EFB at GFX_SCALE. Its RAM copy is written only when something reads it (a texture
 *    loaded from it, an off-screen pass): the EFB rows drawn since the last write are copied to a texture at half
 *    size (2x2 box filter) and converted to RGBA5551 on the CPU.
 *  - Off-screen color images (motion blur, pause/picto/transition captures, VisFbuf, Lens of Truth, coverage) are
 *    drawn at 1x into the EFB's top-left 320x240, the canvas. When a pass starts, the frame's color and depth there
 *    are copied to textures; when it ends they are drawn back (depth through a Z texture). The canvas is loaded
 *    from RAM only before a draw that needs the old pixels; drawn pixels go to RAM when the pass ends or when
 *    something reads them.
 *  - At the end of a task that cleared the z-buffer, its RAM gets the frame's depth in the N64 format, one value
 *    per 4x4 pixels, for the game's SysCfb_GetZBufferPixel readers (sun lens flare, light glows).
 */
#include <gccore.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>
#include "gc_ogc.h"
#include "gfx_internal.h"

/* Off-screen images and frame copies are N64 sized at most */
#define FB_W GFX_N64_WIDTH
#define FB_H GFX_N64_HEIGHT

/* N64 images live in game RAM (MEM1) */
#define FB_RAM_START 0x80000000u
#define FB_RAM_END 0x81800000u

/* Write the frame's depth to the z-buffer's RAM at the end of each task that cleared it */
#ifndef GFX_FB_DEPTH
#define GFX_FB_DEPTH 1
#endif

#define FB_LOG_LIMIT 8

/* gGfxFb.dirtyY0 when no frame row is pending: gfx_fb_frame_drawn's min/max then starts from the first draw's rows
 * (with 0 the range would always grow from row 0) */
#define FB_ROWS_NONE 0x7FFF

typedef struct {
    int x0, y0, x1, y1; /* x0 >= x1 or y0 >= y1: empty */
} FbRect;

GfxFbState gGfxFb;

static bool sReady;   /* EFB copies reach RAM: readbacks, off-screen passes, depth */
static bool sBuffers; /* buffers allocated: GPU-only uses (gfx_fb_frame_texture) work even without sReady */
static u8* sXfer;      /* FB_W x FB_H x 2 bytes: EFB copies on their way to RAM, images on their way to the canvas */
static u8* sSaveColor; /* FB_W x FB_H RGBA8: the frame's pixels under the canvas */
static u8* sSaveDepth; /* FB_W x FB_H Z24X8: their depth; at task end, quarters of the EFB depth (640x120) */

/* The off-screen image being drawn */
static struct {
    bool active; /* the canvas covers the frame's top-left corner (saved in sSaveColor/sSaveDepth) */
    u32 key;     /* image address (KSEG0) */
    u8 siz;      /* G_IM_SIZ_16b (RGBA5551) or G_IM_SIZ_8b (I8) */
    u16 width;   /* pixels per row in RAM, also the canvas width */
    bool loaded; /* the whole canvas holds the image (loaded from RAM, drawn over since) */
    FbRect dirty; /* drawn since the last write to RAM */
} sCanvas;

static GfxFbStats sStats;
static int sLogCount, sLogPasses;

static inline u32 fb_key(u32 addr) {
    return (addr == 0) ? 0 : ((addr & 0x1FFFFFFF) | 0x80000000);
}

static inline bool fb_rect_empty(const FbRect* r) {
    return r->x0 >= r->x1 || r->y0 >= r->y1;
}

/* Whether RAM [addr, addr + bytes) is game memory */
static inline bool fb_ram_ok(u32 addr, u32 bytes) {
    return addr >= FB_RAM_START && addr < FB_RAM_END && bytes <= FB_RAM_END - addr;
}

static bool fb_probe_copies(void);

void gfx_fb_init(void) {
    unsigned int need = FB_W * FB_H * (2 + 4 + 4) + 3 * 32;
    unsigned int arenaFree = (unsigned int)SYS_GetArena1Hi() - (unsigned int)SYS_GetArena1Lo();

    gGfxFb.dirtyY0 = FB_ROWS_NONE;
    gGfxFb.dirtyY1 = 0;
    // The game's threads and buffers still need about 1.5 MiB of the arena after this (see gfx_task.c)
    if (arenaFree < need + 1536 * 1024) {
        gc_log("gfx: fb: %u KB free in MEM1, framebuffer effects need %u KB: off", arenaFree / 1024, need / 1024);
        return;
    }
    sXfer = gc_mem_alloc(FB_W * FB_H * 2, 32);
    sSaveColor = gc_mem_alloc(FB_W * FB_H * 4, 32);
    sSaveDepth = gc_mem_alloc(FB_W * FB_H * 4, 32);
    if (sXfer == NULL || sSaveColor == NULL || sSaveDepth == NULL) {
        gc_log("gfx: fb: no memory for the framebuffer buffers: framebuffer effects off");
        return;
    }
    // EFB copies write these buffers behind the data cache: lines the arena's earlier users left dirty must not be
    // written back over a copy later
    DCFlushRange(sXfer, FB_W * FB_H * 2);
    DCFlushRange(sSaveColor, FB_W * FB_H * 4);
    DCFlushRange(sSaveDepth, FB_W * FB_H * 4);
    sBuffers = true;
    if (!fb_probe_copies()) {
        gc_log("gfx: fb: EFB copies do not reach RAM: framebuffer effects off (Dolphin: turn off Graphics > Hacks > "
               "Store EFB Copies to Texture Only, Graphics.Hacks.EFBToTextureEnable=False)");
        return;
    }
    sReady = true;
    gc_log("gfx: fb: framebuffer effects ready (%u KB)", need / 1024);
}

bool gfx_fb_ready(void) {
    return sReady;
}

void gfx_fb_task_begin(void) {
    gGfxFb.frameKey = 0;
    gGfxFb.frameWidth = 0;
    gGfxFb.dirtyY0 = FB_ROWS_NONE;
    gGfxFb.dirtyY1 = 0;
    gGfxFb.canvasDirty = false;
    sCanvas.active = false;
}

void gfx_fb_set_frame(uint32_t key, uint16_t width) {
    gGfxFb.frameKey = key;
    gGfxFb.frameWidth = width;
}

bool gfx_fb_canvas_active(void) {
    return sCanvas.active;
}

/* ============================================================================================== */
/* EFB copies                                                                                     */
/* ============================================================================================== */

/* Texture copies sample each pixel at its center, without the video mode's vertical (deflicker) filter, which
 * GX applies to texture copies too; the display copy gets the mode's filter back afterwards */
static u8 sCopyPattern[12][2] = {
    { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
    { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 }, { 6, 6 },
};
static u8 sCopyVFilter[7] = { 0, 0, 21, 22, 21, 0, 0 };

static void fb_copy_begin(void) {
    gfx_gx_flush();
    GX_SetCopyFilter(GX_FALSE, sCopyPattern, GX_TRUE, sCopyVFilter);
}

static void fb_copy_end(void) {
    GXRModeObj* mode = (GXRModeObj*)gc_ogc_video_mode();

    GX_SetCopyFilter(mode->aa, mode->sample_pattern, GX_TRUE, mode->vfilter);
}

/* Wait until GX has executed everything queued (the draws of the task so far, then the copy) */
static void fb_wait(void) {
    u64 start = gettime();

    GX_DrawDone();
    sStats.waitTicks += gettime() - start;
}

/* Copy EFB [x, x + w) x [y, y + h) (even values) to sXfer as `fmt`, halved by a box filter if `half`, and wait
 * until it is in memory. Returns the texture's width in texels (rows of tiles). */
static int fb_copy_to_xfer(int x, int y, int w, int h, u32 fmt, bool half) {
    int tw = half ? w / 2 : w;
    int th = half ? h / 2 : h;
    int tileW = (fmt == GX_CTF_R8) ? 8 : 4;

    tw = (tw + tileW - 1) & ~(tileW - 1);
    GX_SetTexCopySrc(x, y, w, h);
    GX_SetTexCopyDst(tw, th, fmt, half ? GX_TRUE : GX_FALSE);
    GX_CopyTex(sXfer, GX_FALSE);
    GX_PixModeSync();
    fb_wait();
    // Tiles are 4 rows high
    DCInvalidateRange(sXfer, tw * ((th + 3) & ~3) * ((fmt == GX_CTF_R8) ? 1 : 2));
    return tw;
}

/* Whether EFB copies reach RAM where the CPU can read them. They always do on the GameCube; Dolphin's "Store EFB
 * Copies to Texture Only" (on by default) keeps them on the host GPU, and every readback would read stale memory.
 * The EFB's top-left 8x8 is cleared to a known color by one copy and read by a second. */
static bool fb_probe_copies(void) {
    static const GXColor probe = { 0x48, 0x90, 0xD8, 0xFF };
    static const GXColor black = { 0, 0, 0, 255 };
    const u16* t = (const u16*)sXfer;
    u16 expect = ((0x48 >> 3) << 11) | ((0x90 >> 2) << 5) | (0xD8 >> 3);
    bool ok;

    memset(sXfer, 0x5A, 64);
    DCFlushRange(sXfer, 64);
    fb_copy_begin();
    GX_SetCopyClear(probe, GX_MAX_Z24);
    GX_SetTexCopySrc(0, 0, 8, 8);
    GX_SetTexCopyDst(8, 8, GX_TF_RGB565, GX_FALSE);
    GX_CopyTex(sXfer + 64, GX_TRUE);
    GX_PixModeSync();
    GX_CopyTex(sXfer, GX_FALSE);
    GX_PixModeSync();
    GX_SetCopyClear(black, GX_MAX_Z24);
    fb_wait();
    fb_copy_end();
    DCInvalidateRange(sXfer, 64);
    ok = t[0] == expect && t[15] == expect;
    if (!ok) {
        gc_log("gfx: fb: EFB copy probe read %04X %04X, expected %04X", t[0], t[15], expect);
    }
    return ok;
}

/* ============================================================================================== */
/* Pixel conversions                                                                              */
/* ============================================================================================== */

/* Conversions run over tile rows: 4 RGB565 texels (two words) or 8 R8/I8 texels at a time when the rows line up
 * with the tiles, one texel at a time at unaligned edges. */

/* Two RGB565 texels -> two RGBA5551 pixels: R and the top 5 bits of G are in place, B moves up one bit. The EFB has
 * no alpha: pixels come back with the coverage/alpha bit set, as the RDP writes drawn pixels. */
static inline u32 fb_565_to_5551x2(u32 v) {
    return (v & 0xFFC0FFC0) | ((v << 1) & 0x003E003E) | 0x00010001;
}

/* Two RGBA5551 pixels -> two RGB565 texels: G gets a sixth bit (its top bit repeated), B moves down one bit */
static inline u32 fb_5551_to_565x2(u32 p) {
    return (p & 0xFFC0FFC0) | ((p >> 5) & 0x00200020) | ((p >> 1) & 0x001F001F);
}

/* GX RGB565 tiles (4x4 texels) of a texture `tw` texels wide -> RGBA5551 rows; texel (sx, sy) goes to dst[0] */
static void fb_rgb565_to_rgba16(int tw, int sx, int sy, int w, int h, u16* dst, int dstStride) {
    const u16* tiles = (const u16*)sXfer;
    int x, y;

    for (y = 0; y < h; y++) {
        int ty = sy + y;
        const u16* row = tiles + (ty >> 2) * (tw / 4) * 16 + (ty & 3) * 4;
        u16* out = dst + y * dstStride;

        x = 0;
        if ((sx & 3) == 0 && ((u32)out & 3) == 0) {
            const u32* in = (const u32*)(row + sx * 4);
            u32* o = (u32*)out;

            for (; x + 4 <= w; x += 4, in += 8, o += 2) {
                o[0] = fb_565_to_5551x2(in[0]);
                o[1] = fb_565_to_5551x2(in[1]);
            }
        }
        for (; x < w; x++) {
            int tx = sx + x;
            u32 c = row[(tx >> 2) * 16 + (tx & 3)];

            out[x] = (c & 0xFFC0) | ((c & 0x1F) << 1) | 1;
        }
    }
}

/* GX R8 tiles (8x4 texels) -> I8 rows (the RDP writes red to 8-bit color images) */
static void fb_r8_to_i8(int tw, int sx, int sy, int w, int h, u8* dst, int dstStride) {
    const u8* tiles = sXfer;
    int x, y;

    for (y = 0; y < h; y++) {
        int ty = sy + y;
        const u8* row = tiles + (ty >> 2) * (tw / 8) * 32 + (ty & 3) * 8;
        u8* out = dst + y * dstStride;

        x = 0;
        if ((sx & 7) == 0 && ((u32)out & 3) == 0) {
            const u32* in = (const u32*)(row + sx * 4);
            u32* o = (u32*)out;

            for (; x + 8 <= w; x += 8, in += 8, o += 2) {
                o[0] = in[0];
                o[1] = in[1];
            }
        }
        for (; x < w; x++) {
            int tx = sx + x;

            out[x] = row[(tx >> 3) * 32 + (tx & 7)];
        }
    }
}

/* RGBA5551 rows -> GX RGB565 tiles, `tw` (multiple of 4) x h texels */
static void fb_rgba16_to_rgb565(const u16* src, int srcStride, int tw, int w, int h) {
    u16* tiles = (u16*)sXfer;
    int x, y;

    for (y = 0; y < h; y++) {
        u16* row = tiles + (y >> 2) * (tw / 4) * 16 + (y & 3) * 4;
        const u16* in = src + y * srcStride;

        x = 0;
        if (((u32)in & 3) == 0) {
            const u32* i = (const u32*)in;
            u32* o = (u32*)row;

            for (; x + 4 <= w; x += 4, i += 2, o += 8) {
                o[0] = fb_5551_to_565x2(i[0]);
                o[1] = fb_5551_to_565x2(i[1]);
            }
        }
        for (; x < w; x++) {
            u32 p = in[x];

            row[(x >> 2) * 16 + (x & 3)] = (p & 0xFFC0) | ((p >> 5) & 0x20) | ((p >> 1) & 0x1F);
        }
    }
}

/* I8 rows -> GX I8 tiles (8x4), `tw` (multiple of 8) x h texels */
static void fb_i8_to_tiles(const u8* src, int srcStride, int tw, int w, int h) {
    u8* tiles = sXfer;
    int x, y;

    for (y = 0; y < h; y++) {
        u8* row = tiles + (y >> 2) * (tw / 8) * 32 + (y & 3) * 8;
        const u8* in = src + y * srcStride;

        x = 0;
        if (((u32)in & 3) == 0) {
            const u32* i = (const u32*)in;
            u32* o = (u32*)row;

            for (; x + 8 <= w; x += 8, i += 2, o += 8) {
                o[0] = i[0];
                o[1] = i[1];
            }
        }
        for (; x < w; x++) {
            row[(x >> 3) * 32 + (x & 7)] = in[x];
        }
    }
}

/* ============================================================================================== */
/* Quads drawn by gfx_fb.c (GX_TEXMAP6/7 only: textures the caller bound to GX_TEXMAP0/1 survive)  */
/* ============================================================================================== */

static void fb_draw_begin(GXTexObj* color, GXTexObj* depth) {
    Mtx44 proj;

    gfx_gx_flush();
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT1, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetNumIndStages(0);

    GX_SetTevDirect(GX_TEVSTAGE0);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP7, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    if (depth != NULL) {
        // The Z texture is the last stage's texture: it replaces the quad's depth
        GX_SetTevDirect(GX_TEVSTAGE1);
        GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD0, GX_TEXMAP6, GX_COLORNULL);
        GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_CPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
        GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetNumTevStages(2);
        GX_SetZTexture(GX_ZT_REPLACE, GX_TF_Z24X8, 0);
        GX_SetZCompLoc(GX_FALSE);
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    } else {
        GX_SetNumTevStages(1);
        GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    }
    GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, (GXColor){ 0, 0, 0, 0 });
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);

    // EFB pixels, y down
    guOrtho(proj, 0, GFX_EFB_HEIGHT, 0, GFX_EFB_WIDTH, 0, 1);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    GX_SetViewport(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT, 0, 1);
    GX_SetScissor(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);

    // The textures were just written by EFB copies or the CPU
    GX_InvalidateTexAll();
    GX_LoadTexObj(color, GX_TEXMAP7);
    if (depth != NULL) {
        GX_LoadTexObj(depth, GX_TEXMAP6);
    }
}

/* EFB pixels [x0, x1) x [y0, y1), texture [0, u1) x [0, v1). z -0.5 is inside the orthographic clip range. */
static void fb_quad(float x0, float y0, float x1, float y1, float u1, float v1) {
    GX_Begin(GX_QUADS, GX_VTXFMT1, 4);
    GX_Position3f32(x0, y0, -0.5f);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, y0, -0.5f);
    GX_TexCoord2f32(u1, 0.0f);
    GX_Position3f32(x1, y1, -0.5f);
    GX_TexCoord2f32(u1, v1);
    GX_Position3f32(x0, y1, -0.5f);
    GX_TexCoord2f32(0.0f, v1);
    GX_End();
}

static void fb_draw_end(bool depth) {
    if (depth) {
        GX_SetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    }
    gfx_gx_state_lost();
}

/* ============================================================================================== */
/* The frame                                                                                      */
/* ============================================================================================== */

/* Write the frame's pending rows to RAM: EFB rows [2 y0, 2 y1) at half size */
static void fb_frame_write(void) {
    int y0 = gGfxFb.dirtyY0, y1 = gGfxFb.dirtyY1;
    u32 key = gGfxFb.frameKey;
    int tw;

    gGfxFb.dirtyY0 = FB_ROWS_NONE;
    gGfxFb.dirtyY1 = 0;
    y0 = (y0 < 0) ? 0 : y0;
    y1 = (y1 > FB_H) ? FB_H : y1;
    if (!sReady || y0 >= y1 || key == 0) {
        return;
    }
    if (sCanvas.active) {
        // The canvas covers part of the frame (its rows were written when the pass started; nothing draws into
        // the frame during a pass)
        return;
    }
    if (gGfxFb.frameWidth != FB_W || !fb_ram_ok(key, FB_W * FB_H * 2)) {
        if (sLogCount < FB_LOG_LIMIT) {
            sLogCount++;
            gc_log("gfx: fb: frame %08X (%u wide) not written to RAM", (unsigned int)key, gGfxFb.frameWidth);
        }
        return;
    }
    fb_copy_begin();
    tw = fb_copy_to_xfer(0, y0 * GFX_SCALE, GFX_EFB_WIDTH, (y1 - y0) * GFX_SCALE, GX_TF_RGB565, GFX_SCALE == 2);
    fb_copy_end();
    fb_rgb565_to_rgba16(tw, 0, 0, FB_W, y1 - y0, (u16*)key + y0 * FB_W, FB_W);
    gfx_tex_ram_written(key + y0 * FB_W * 2, (y1 - y0) * FB_W * 2);
    sStats.readbacks++;
    sStats.readbackRows += y1 - y0;
}

/* ============================================================================================== */
/* The canvas                                                                                     */
/* ============================================================================================== */

static inline u32 fb_canvas_bpp(void) {
    return (sCanvas.siz == G_IM_SIZ_8b) ? 1 : 2;
}

/* Write the canvas pixels drawn since the last write to the image's RAM */
static void fb_canvas_write(void) {
    FbRect d = sCanvas.dirty;
    u32 bpp = fb_canvas_bpp();
    u32 rowBytes = sCanvas.width * bpp;
    int sx0, sy0, sx1, sy1, tw;
    u32 first;

    sCanvas.dirty.x0 = sCanvas.dirty.x1 = 0;
    gGfxFb.canvasDirty = false;
    if (fb_rect_empty(&d)) {
        return;
    }
    first = sCanvas.key + d.y0 * rowBytes;
    if (!fb_ram_ok(first, (d.y1 - d.y0) * rowBytes)) {
        return;
    }
    // EFB copies start and end on even pixels
    sx0 = d.x0 & ~1;
    sy0 = d.y0 & ~1;
    sx1 = (d.x1 + 1) & ~1;
    sy1 = (d.y1 + 1) & ~1;
    fb_copy_begin();
    tw = fb_copy_to_xfer(sx0, sy0, sx1 - sx0, sy1 - sy0, (bpp == 1) ? GX_CTF_R8 : GX_TF_RGB565, false);
    fb_copy_end();
    if (bpp == 1) {
        fb_r8_to_i8(tw, d.x0 - sx0, d.y0 - sy0, d.x1 - d.x0, d.y1 - d.y0, (u8*)first + d.x0, rowBytes);
    } else {
        fb_rgb565_to_rgba16(tw, d.x0 - sx0, d.y0 - sy0, d.x1 - d.x0, d.y1 - d.y0, (u16*)first + d.x0, sCanvas.width);
    }
    gfx_tex_ram_written(first, (d.y1 - d.y0) * rowBytes);
    sStats.copyOuts++;
}

/* Draw the image from RAM into the whole canvas */
static void fb_canvas_load(void) {
    u32 bpp = fb_canvas_bpp();
    u32 rowBytes = sCanvas.width * bpp;
    int w = sCanvas.width, h = FB_H;
    int tw = (bpp == 1) ? ((w + 7) & ~7) : ((w + 3) & ~3);
    GXTexObj obj;

    // Rows past the end of RAM stay as they are in the canvas (never drawn by the game anyway)
    while (h > 0 && !fb_ram_ok(sCanvas.key, h * rowBytes)) {
        h--;
    }
    if (h == 0) {
        return;
    }
    if (h != FB_H || tw != w) {
        memset(sXfer, 0, FB_W * FB_H * 2);
    }
    if (bpp == 1) {
        fb_i8_to_tiles((const u8*)sCanvas.key, rowBytes, tw, w, h);
        GX_InitTexObj(&obj, sXfer, tw, (h + 3) & ~3, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    } else {
        fb_rgba16_to_rgb565((const u16*)sCanvas.key, sCanvas.width, tw, w, h);
        GX_InitTexObj(&obj, sXfer, tw, (h + 3) & ~3, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    DCFlushRange(sXfer, FB_W * FB_H * 2);
    GX_InitTexObjFilterMode(&obj, GX_NEAR, GX_NEAR);
    fb_draw_begin(&obj, NULL);
    fb_quad(0, 0, w, h, (float)w / tw, (float)h / ((h + 3) & ~3));
    fb_draw_end(false);
    sStats.uploads++;
}

/* Copy the frame's color and depth under the canvas */
static void fb_canvas_save(void) {
    fb_copy_begin();
    GX_SetTexCopySrc(0, 0, FB_W, FB_H);
    GX_SetTexCopyDst(FB_W, FB_H, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(sSaveColor, GX_FALSE);
    GX_SetTexCopyDst(FB_W, FB_H, GX_TF_Z24X8, GX_FALSE);
    GX_CopyTex(sSaveDepth, GX_FALSE);
    GX_PixModeSync();
    fb_copy_end();
}

/* Draw the frame's saved color and depth back */
static void fb_canvas_restore(void) {
    GXTexObj color, depth;

    GX_InitTexObj(&color, sSaveColor, FB_W, FB_H, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&color, GX_NEAR, GX_NEAR);
    GX_InitTexObj(&depth, sSaveDepth, FB_W, FB_H, GX_TF_Z24X8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&depth, GX_NEAR, GX_NEAR);
    fb_draw_begin(&color, &depth);
    fb_quad(0, 0, FB_W, FB_H, 1.0f, 1.0f);
    fb_draw_end(true);
}

bool gfx_fb_canvas_begin(uint32_t key, uint8_t fmt, uint8_t siz, uint16_t width) {
    u64 start;

    if (!sReady || key == 0) {
        return false;
    }
    if ((siz != G_IM_SIZ_16b && siz != G_IM_SIZ_8b) || width == 0 || width > FB_W) {
        if (sLogCount < FB_LOG_LIMIT) {
            sLogCount++;
            gc_log("gfx: fb: off-screen color image %08X (fmt %u siz %u, %u wide) not supported, its draws are "
                   "skipped", (unsigned int)key, fmt, siz, width);
        }
        return false;
    }
    if (sCanvas.active && sCanvas.key == key && sCanvas.siz == siz && sCanvas.width == width) {
        return true;
    }
    start = gettime();
    if (sCanvas.active) {
        fb_canvas_write();
    } else {
        // Off-screen passes usually copy the frame: its pending rows go to RAM while they are all in the EFB
        fb_frame_write();
        fb_canvas_save();
        sCanvas.active = true;
        sStats.passes++;
    }
    if (sLogPasses < FB_LOG_LIMIT) {
        sLogPasses++;
        gc_log("gfx: fb: off-screen color image %08X (siz %u, %u wide), frame %08X", (unsigned int)key, siz, width,
               (unsigned int)gGfxFb.frameKey);
    }
    sCanvas.key = key;
    sCanvas.siz = siz;
    sCanvas.width = width;
    sCanvas.loaded = false;
    sCanvas.dirty.x0 = sCanvas.dirty.x1 = 0;
    sStats.ticks += gettime() - start;
    return true;
}

/* Whether the union of two rectangles is a rectangle */
static bool fb_union_exact(const FbRect* a, const FbRect* b) {
    if (a->x0 <= b->x0 && a->y0 <= b->y0 && a->x1 >= b->x1 && a->y1 >= b->y1) {
        return true;
    }
    if (b->x0 <= a->x0 && b->y0 <= a->y0 && b->x1 >= a->x1 && b->y1 >= a->y1) {
        return true;
    }
    if (a->x0 == b->x0 && a->x1 == b->x1) {
        return b->y0 <= a->y1 && a->y0 <= b->y1;
    }
    if (a->y0 == b->y0 && a->y1 == b->y1) {
        return b->x0 <= a->x1 && a->x0 <= b->x1;
    }
    return false;
}

void gfx_fb_canvas_draw(int x0, int y0, int x1, int y1, bool opaque) {
    FbRect r = { (x0 < 0) ? 0 : x0, (y0 < 0) ? 0 : y0, (x1 > sCanvas.width) ? sCanvas.width : x1,
                 (y1 > FB_H) ? FB_H : y1 };
    FbRect* d = &sCanvas.dirty;
    u64 start;

    if (!sCanvas.active || fb_rect_empty(&r)) {
        return;
    }
    if (!sCanvas.loaded && (!opaque || (!fb_rect_empty(d) && !fb_union_exact(d, &r)))) {
        // Only drawn pixels may go back to RAM: until the canvas is loaded, the dirty rectangle must be exactly the
        // pixels drawn. A draw that reads the old pixels needs the canvas loaded, with what was drawn so far in RAM.
        start = gettime();
        fb_canvas_write();
        if (!opaque) {
            fb_canvas_load();
            sCanvas.loaded = true;
        }
        sStats.ticks += gettime() - start;
    }
    if (fb_rect_empty(d)) {
        *d = r;
    } else {
        d->x0 = (r.x0 < d->x0) ? r.x0 : d->x0;
        d->y0 = (r.y0 < d->y0) ? r.y0 : d->y0;
        d->x1 = (r.x1 > d->x1) ? r.x1 : d->x1;
        d->y1 = (r.y1 > d->y1) ? r.y1 : d->y1;
    }
    gGfxFb.canvasDirty = true;
}

void gfx_fb_canvas_end(void) {
    u64 start;

    if (!sCanvas.active) {
        return;
    }
    start = gettime();
    fb_canvas_write();
    fb_canvas_restore();
    sCanvas.active = false;
    gGfxFb.canvasDirty = false;
    sStats.ticks += gettime() - start;
}

bool gfx_fb_frame_texture(GXTexObj* out) {
    if (!sBuffers || sCanvas.active) {
        return false;
    }
    // sSaveColor is free outside off-screen passes
    fb_copy_begin();
    GX_SetTexCopySrc(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    GX_SetTexCopyDst(FB_W, FB_H, GX_TF_RGB565, GFX_SCALE == 2 ? GX_TRUE : GX_FALSE);
    GX_CopyTex(sSaveColor, GX_FALSE);
    GX_PixModeSync();
    fb_copy_end();
    GX_InvalidateTexAll();
    GX_InitTexObj(out, sSaveColor, FB_W, FB_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(out, GX_NEAR, GX_NEAR);
    return true;
}

/* ============================================================================================== */
/* Reads                                                                                          */
/* ============================================================================================== */

void gfx_fb_sync_ram(const void* addr, uint32_t bytes) {
    u32 a = fb_key((u32)addr);
    u32 b = a + bytes;
    u64 start;

    // Batched draws go to GX first: they may draw into the range, and the caller is about to bind a texture
    gfx_gx_flush();
    // Without working EFB copies (or without sXfer) nothing is written back, as before gfx_fb.c; gfx_gx.c still
    // records frame rows
    if (!sReady || !gfx_fb_pending() || a == 0) {
        return;
    }
    start = gettime();
    if (gGfxFb.dirtyY0 < gGfxFb.dirtyY1 && gGfxFb.frameKey != 0 && gGfxFb.frameWidth != 0) {
        u32 f0 = gGfxFb.frameKey;
        u32 rowBytes = gGfxFb.frameWidth * 2;
        u32 f1 = f0 + rowBytes * FB_H;

        if (a < f1 && f0 < b) {
            int y0 = (int)(((a > f0) ? a - f0 : 0) / rowBytes);
            int y1 = (int)((((b < f1) ? b : f1) - f0 + rowBytes - 1) / rowBytes);

            if (y0 < gGfxFb.dirtyY1 && gGfxFb.dirtyY0 < y1) {
                fb_frame_write();
            }
        }
    }
    if (gGfxFb.canvasDirty && sCanvas.active) {
        u32 c0 = sCanvas.key;
        u32 c1 = c0 + sCanvas.width * fb_canvas_bpp() * FB_H;

        if (a < c1 && c0 < b) {
            fb_canvas_write();
        }
    }
    sStats.ticks += gettime() - start;
}

/* ============================================================================================== */
/* Depth                                                                                          */
/* ============================================================================================== */

/* GX window depth (24 bits; gfx_gx.c makes it the N64's screen z / G_MAXZ) as an N64 z-buffer value: the RDP's
 * 18-bit z (screen z << 8) compressed to a 3-bit exponent (leading ones) and an 11-bit mantissa, dz 0. Never drawn
 * pixels give the game's clear value (G_MAXFBZ), which the lens flare test looks for. */
static inline u16 fb_n64_depth(u32 d) {
    u32 z, e;

    if (d >= 0xFFFFFF) {
        return 0xFFFC;
    }
    z = ((d >> 4) * G_MAXZ) >> 12;
    e = __builtin_clz(~(z << 14));
    if (e >= 7) {
        return (7 << 13) | ((z & 0x7FF) << 2);
    }
    return (e << 13) | (((z >> (6 - e)) & 0x7FF) << 2);
}

/* Depth of texel i (0..15) of a Z24X8 tile: 64 bytes, the first 32 hold (X, Z[23:16]) pairs, the next 32 Z[15:0] */
static inline u32 fb_z24(const u8* tile, int i) {
    return ((u32)tile[i * 2 + 1] << 16) | ((const u16*)(tile + 32))[i];
}

/* EFB rows per depth copy: a quarter of the frame, 1:1 as Z24X8, fills sSaveDepth */
#define FB_DEPTH_ROWS (GFX_EFB_HEIGHT / 4)

/* The frame's depth into the z-buffer's RAM (320 x 240, 16 bits per pixel), one value per 4x4 pixels: the EFB depth
 * at N64 pixel (1, 1) of each 4x4 block, converted, fills the block. The game reads single pixels and compares them
 * loosely (whether the sun is hidden, whether a light is behind something). Depth is copied without the 2x2 box
 * filter (it would average the 24-bit values channel by channel), a quarter of the EFB at a time. */
static void fb_depth_write(u16* z) {
    const int tilesW = GFX_EFB_WIDTH / 4; /* Z24X8 tiles (4x4 texels, 64 bytes) per copied row */
    int q, tx, ty;

    for (q = 0; q < GFX_EFB_HEIGHT / FB_DEPTH_ROWS; q++) {
        fb_copy_begin();
        GX_SetTexCopySrc(0, q * FB_DEPTH_ROWS, GFX_EFB_WIDTH, FB_DEPTH_ROWS);
        GX_SetTexCopyDst(GFX_EFB_WIDTH, FB_DEPTH_ROWS, GX_TF_Z24X8, GX_FALSE);
        GX_CopyTex(sSaveDepth, GX_FALSE);
        GX_PixModeSync();
        fb_wait();
        fb_copy_end();

        // 4x4 blocks whose sample row lies in this quarter (only the tiles read leave the data cache)
        for (ty = q * FB_DEPTH_ROWS / (4 * GFX_SCALE); ty < (q + 1) * FB_DEPTH_ROWS / (4 * GFX_SCALE); ty++) {
            int r = (ty * 4 + 1) * GFX_SCALE - q * FB_DEPTH_ROWS;
            u32* row = (u32*)(z + ty * 4 * FB_W);

            for (tx = 0; tx < FB_W / 4; tx++, row += 2) {
                int c = (tx * 4 + 1) * GFX_SCALE;
                const u8* tile = sSaveDepth + ((r >> 2) * tilesW + (c >> 2)) * 64;
                u32 v;

                DCInvalidateRange((void*)tile, 64);
                v = fb_n64_depth(fb_z24(tile, (r & 3) * 4 + (c & 3)));

                v |= v << 16;
                row[0] = row[1] = v;
                row[FB_W / 2] = row[FB_W / 2 + 1] = v;
                row[FB_W] = row[FB_W + 1] = v;
                row[FB_W * 3 / 2] = row[FB_W * 3 / 2 + 1] = v;
            }
        }
    }
    sStats.depthWrites++;
}

void gfx_fb_task_end(const void* zImage, bool writeDepth) {
    u32 key = fb_key((u32)zImage);
    u64 start;

    if (!sReady) {
        return;
    }
    if (sCanvas.active) {
        gfx_fb_canvas_end();
    }
    if (GFX_FB_DEPTH && writeDepth && key != 0 && key != gGfxFb.frameKey && gGfxFb.frameWidth == FB_W &&
        (key & 3) == 0 && fb_ram_ok(key, FB_W * FB_H * 2)) {
        start = gettime();
        fb_depth_write((u16*)key);
        gfx_tex_ram_written(key, FB_W * FB_H * 2);
        sStats.ticks += gettime() - start;
    }
}

void gfx_fb_take_stats(GfxFbStats* out) {
    *out = sStats;
    memset(&sStats, 0, sizeof(sStats));
}

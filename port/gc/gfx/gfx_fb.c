/**
 * Framebuffer effects of the N64 renderer (DESIGN.md, "Framebuffer effects"): N64 color images in RAM versus what
 * GX draws in the EFB.
 *
 *  - The frame is drawn in the EFB at GFX_SCALE. Its RAM copy is written only when something reads it (a texture
 *    loaded from it): the EFB rows drawn since the last write are copied to a texture at half size (2x2 box filter)
 *    and converted to RGBA5551 on the CPU.
 *  - Off-screen color images (motion blur, pause/picto/transition captures, VisFbuf, Lens of Truth, coverage) are
 *    drawn at 1x into the EFB's top-left 320x240, the canvas. When a pass starts, the frame's color and depth there
 *    are copied to textures; when it ends they are drawn back (depth through a Z texture). The frame's pending rows
 *    stay pending: during the pass they are in a copy of the frame made when it started. The canvas is loaded from
 *    RAM only before a draw that needs the old pixels; drawn pixels go to RAM when the pass ends or when something
 *    reads them.
 *  - Images the renderer holds itself are textures without a RAM round trip (gfx_fb_bind_image, for S2DEX2
 *    backgrounds): the frame as an EFB copy, and the last off-screen image written to RAM as the EFB copy that wrote
 *    it. Both are the RGB5A3 texels gfx_tex.c would make of the RAM, so backgrounds read from the frame or from a
 *    capture (motion blur, VisFbuf's screen shrink, the pause and transition captures) need no CPU conversion,
 *    hashing or texture cache entry. Should sXfer be needed again before the background's draws are issued (the draw
 *    switches the render target or loads the canvas after the bind), the binding moves to RAM first.
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

/* Test builds (-DGFX_FB_VERIFY=1 in GFX_CFLAGS): every texture gfx_fb_bind_image serves, every copy that goes to RAM
 * and every depth write is compared with what the renderer made of the same EFB pixels before (RGB565 copies, the
 * RAM path's texels, whole quarters of the EFB depth); "gx: fb: verify:" lines count the texels and differences */
#ifndef GFX_FB_VERIFY
#define GFX_FB_VERIFY 0
#endif

/* Measurement builds (-DGFX_FB_AB=1): every other statistics window (gfx_fb_take_stats) binds backgrounds from RAM and
 * writes the frame to RAM when an off-screen pass starts, as before gfx_fb_bind_image, for comparisons of the same
 * frames in one run (the output is the same either way) */
#ifndef GFX_FB_AB
#define GFX_FB_AB 0
#endif
#if GFX_FB_AB
static bool sAbRam;
#endif

#define FB_LOG_LIMIT 8

/* gGfxFb.dirtyY0 when no frame row is pending: gfx_fb_frame_drawn's min/max then starts from the first draw's rows
 * (with 0 the range would always grow from row 0) */
#define FB_ROWS_NONE 0x7FFF

/* Frame rows written to RAM in this task, one bit each */
#define FB_ROW_WORDS ((FB_H + 31) / 32)

/* An RGBA16 image of FB_W x FB_H pixels as 32-bit words (two pixels each) */
#define FB_IMAGE_WORDS (FB_W * FB_H / 2)

typedef struct {
    int x0, y0, x1, y1; /* x0 >= x1 or y0 >= y1: empty */
} FbRect;

/* Checksum of an RGBA16 image's RAM (fb_sum): the sum of its words, and the XOR of each word rotated by its index
 * modulo 32. Written images get theirs while they are converted, in tile order (the rotation of a word only depends on
 * its column when a row is a multiple of 32 words, as FB_W / 2 is). */
typedef struct {
    u32 add, rot;
} FbSum;

_Static_assert((FB_W / 2) % 32 == 0, "rows of FB_W pixels are whole blocks of 32 words (fb_sum)");

/* What sXfer holds besides transfers: a texture of an N64 image (RGB5A3, opaque texels 1 RRRRR GGGGG BBBBB, which is
 * what gfx_tex.c converts RGBA5551 pixels with the alpha bit set to) */
enum { XFER_NONE, XFER_FRAME, XFER_IMAGE };

GfxFbState gGfxFb;

static bool sReady;   /* EFB copies reach RAM: readbacks, off-screen passes, depth */
static bool sBuffers; /* buffers allocated: GPU-only uses (gfx_fb_frame_texture) work even without sReady */
static u8* sXfer;      /* FB_W x FB_H x 2 bytes: EFB copies on their way to RAM, images on their way to the canvas */
static u8* sSaveColor; /* FB_W x FB_H RGBA8: the frame's pixels under the canvas */
static u8* sSaveDepth; /* FB_W x FB_H Z24X8: their depth; at task end, the EFB depth rows of the z-buffer samples */
static u32 sTask;      /* tasks since boot */

/* Frame rows whose RAM was written from the EFB in this task (the EFB still holds the same pixels unless they were
 * drawn again, which puts them back in gGfxFb's pending rows) */
static u32 sRowsInRam[FB_ROW_WORDS];

/* The off-screen image being drawn */
static struct {
    bool active; /* the canvas covers the frame's top-left corner (saved in sSaveColor/sSaveDepth) */
    u32 key;     /* image address (KSEG0) */
    u8 siz;      /* G_IM_SIZ_16b (RGBA5551) or G_IM_SIZ_8b (I8) */
    u16 width;   /* pixels per row in RAM, also the canvas width */
    bool loaded; /* the whole canvas holds the image (loaded from RAM, drawn over since) */
    FbRect dirty; /* drawn since the last write to RAM */
} sCanvas;

/* The texture in sXfer */
static struct {
    int kind;      /* XFER_* */
    u32 key;       /* the frame or the image (KSEG0 address) */
    u32 gen;       /* XFER_FRAME: gGfxFb.frameGen when it was copied */
    u32 task;      /* XFER_FRAME: the task it was copied in; XFER_IMAGE: the task in which its RAM was known to match */
    FbSum sum;     /* XFER_IMAGE: checksum of the image's RAM as written */
    bool bound;    /* loaded into a texture map since the last wait for GX: draws may still read it */
    bool stale;    /* rewritten since the last GX_InvalidateTexAll */
} sXferTex;

/* The texture gfx_fb_bind_image served last, until gfx_fb_image_done: draws of it may still be to come, so sXfer is not
 * reused before the binding moves to RAM (fb_served_to_ram) */
static struct {
    bool on;
    bool frame; /* XFER_FRAME, else XFER_IMAGE */
    u32 key;
    bool linear;
    int texMap;
    GfxBindImageFn fromRam;
} sServed;

static GfxFbStats sStats;
static struct {
    u32 frameCopies, frameBgs, imageBgs, imageChecks, imageMismatches, movedToRam;
} sTexStats;
static int sLogCount, sLogPasses;

#if GFX_FB_VERIFY
static u8* sVerify; /* FB_W x FB_H x 2: RGB565 copies of what an RGB5A3 copy got */
static struct {
    u32 copies, textures, texels, mismatches, nonOpaque;
} sVer;
static int sVerLogs;
static void fb_verify_copy(int x, int y, int w, int h, bool half, int tw, int th, const char* what);
static void fb_verify_texture(u32 a, bool frame);
static void fb_verify_depth(const u16* z);
#endif

/* N64 RAM addresses (KSEG0) and CPU pointers: the same on the GameCube; host tests keep the RAM in a buffer */
#ifdef GFX_HOST_TEST
extern uintptr_t gGfxHostRamBias;
#define FB_RAM_BIAS gGfxHostRamBias
#else
#define FB_RAM_BIAS 0
#endif

static inline void* fb_ptr(u32 addr) {
    return (void*)((uintptr_t)addr + FB_RAM_BIAS);
}

static inline u32 fb_key(const void* p) {
    u32 addr = (u32)((uintptr_t)p - FB_RAM_BIAS);

    return (p == NULL) ? 0 : ((addr & 0x1FFFFFFF) | 0x80000000);
}

static inline bool fb_rect_empty(const FbRect* r) {
    return r->x0 >= r->x1 || r->y0 >= r->y1;
}

/* Whether RAM [addr, addr + bytes) is game memory */
static inline bool fb_ram_ok(u32 addr, u32 bytes) {
    return addr >= FB_RAM_START && addr < FB_RAM_END && bytes <= FB_RAM_END - addr;
}

static inline bool fb_overlap(u32 a0, u32 a1, u32 b0, u32 b1) {
    return a0 < b1 && b0 < a1;
}

static inline u32 fb_rotl(u32 v, u32 r) {
    return (v << (r & 31)) | (v >> ((32 - r) & 31));
}

/* Zero a 32-byte cache line without reading it from memory: every byte of it is about to be written */
static inline void fb_dcbz(void* p) {
#ifdef GEKKO
    __asm__ volatile("dcbz 0,%0" : : "r"(p) : "memory");
#else
    (void)p;
#endif
}

static bool fb_probe_copies(void);

void gfx_fb_init(void) {
    unsigned int need = FB_W * FB_H * (2 + 4 + 4) + 3 * 32;
    unsigned int arenaFree = (unsigned int)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());

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
#if GFX_FB_VERIFY
    sVerify = gc_mem_alloc(FB_W * FB_H * 2, 32);
    if (sVerify != NULL) {
        DCFlushRange(sVerify, FB_W * FB_H * 2);
    }
    gc_log("gfx: fb: verification build: textures and RAM writes are compared with RGB565 copies (%s)",
           (sVerify != NULL) ? "on" : "no memory, off");
#endif
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
    sServed.on = false;
    memset(sRowsInRam, 0, sizeof(sRowsInRam));
    // (an XFER_FRAME texture is of the last task's frame now: its task no longer matches)
    sTask++;
}

void gfx_fb_set_frame(uint32_t key, uint16_t width) {
    gGfxFb.frameKey = key;
    gGfxFb.frameWidth = width;
    memset(sRowsInRam, 0, sizeof(sRowsInRam));
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
    sXferTex.bound = false;
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
    sXferTex.stale = true;
    return ok;
}

/* ============================================================================================== */
/* Pixel conversions                                                                              */
/* ============================================================================================== */

/* Conversions run over tile rows: 4 RGB5A3 texels (two words) or 8 R8/I8 texels at a time when the rows line up
 * with the tiles, one texel at a time at unaligned edges; whole tiles at a time when the rectangle is tile aligned. */

/* Two opaque RGB5A3 texels (1 RRRRR GGGGG BBBBB: EFB copies of the EFB, which has no alpha) -> two RGBA5551 pixels: the
 * color moves up one bit and the coverage/alpha bit is set, as the RDP writes drawn pixels */
static inline u32 fb_5a3_to_5551x2(u32 v) {
    return ((v << 1) & 0xFFFEFFFE) | 0x00010001;
}

/* Two RGBA5551 pixels -> two RGB565 texels: G gets a sixth bit (its top bit repeated), B moves down one bit */
static inline u32 fb_5551_to_565x2(u32 p) {
    return (p & 0xFFC0FFC0) | ((p >> 5) & 0x00200020) | ((p >> 1) & 0x001F001F);
}

/* Checksum of `words` 32-bit words (a multiple of 32), see FbSum */
static FbSum fb_sum(const u32* p, u32 words) {
    u32 a0 = 0, a1 = 0, r0 = 0, r1 = 0;
    u32 i;

#define FB_SUM2(k) \
    a0 += p[k];    \
    a1 += p[(k) + 1]; \
    r0 ^= fb_rotl(p[k], (k)); \
    r1 ^= fb_rotl(p[(k) + 1], (k) + 1);

    for (i = 0; i < words; i += 32, p += 32) {
        __builtin_prefetch(p + 64);
        __builtin_prefetch(p + 72);
        __builtin_prefetch(p + 80);
        __builtin_prefetch(p + 88);
        FB_SUM2(0) FB_SUM2(2) FB_SUM2(4) FB_SUM2(6) FB_SUM2(8) FB_SUM2(10) FB_SUM2(12) FB_SUM2(14)
        FB_SUM2(16) FB_SUM2(18) FB_SUM2(20) FB_SUM2(22) FB_SUM2(24) FB_SUM2(26) FB_SUM2(28) FB_SUM2(30)
    }
#undef FB_SUM2
    return (FbSum){ a0 + a1, r0 ^ r1 };
}

/*
 * GX RGB5A3 tiles (4x4 texels) of a texture `tw` texels wide -> RGBA5551 rows; texel (sx, sy) goes to dst[0]. With
 * `sum` (the rectangle is a whole image of FB_W pixels per row) the image's checksum is computed on the way.
 */
static void fb_rgb5a3_to_rgba16(int tw, int sx, int sy, int w, int h, u16* dst, int dstStride, FbSum* sum) {
    const u16* tiles = (const u16*)sXfer;
    int x, y;

    if (((sx | sy | h) & 3) == 0 && (w & 15) == 0 && ((uintptr_t)dst & 31) == 0 && ((dstStride * 2) & 31) == 0) {
        // Whole tiles: each one is a cache line of sXfer, read once; the destination lines are whole (dcbz)
        const u32* t0 = (const u32*)sXfer + ((sy >> 2) * (tw >> 2) + (sx >> 2)) * 8;
        int rowWords = dstStride / 2;
        u32 add = 0, rot = 0;

        for (y = 0; y < h; y += 4, t0 += (tw >> 2) * 8) {
            const u32* t = t0;
            u32* d = (u32*)(dst + y * dstStride);
            int k;

            for (k = 0; k < w / 4; k++, t += 8, d += 2) {
                u32 v0 = fb_5a3_to_5551x2(t[0]), v1 = fb_5a3_to_5551x2(t[1]);
                u32 v2 = fb_5a3_to_5551x2(t[2]), v3 = fb_5a3_to_5551x2(t[3]);
                u32 v4 = fb_5a3_to_5551x2(t[4]), v5 = fb_5a3_to_5551x2(t[5]);
                u32 v6 = fb_5a3_to_5551x2(t[6]), v7 = fb_5a3_to_5551x2(t[7]);

                __builtin_prefetch(t + 32);
                if ((k & 3) == 0) {
                    fb_dcbz(d);
                    fb_dcbz(d + rowWords);
                    fb_dcbz(d + rowWords * 2);
                    fb_dcbz(d + rowWords * 3);
                }
                d[0] = v0;
                d[1] = v1;
                d[rowWords] = v2;
                d[rowWords + 1] = v3;
                d[rowWords * 2] = v4;
                d[rowWords * 2 + 1] = v5;
                d[rowWords * 3] = v6;
                d[rowWords * 3 + 1] = v7;
                if (sum != NULL) {
                    add += v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7;
                    rot ^= fb_rotl(v0 ^ v2 ^ v4 ^ v6, (u32)k * 2) ^ fb_rotl(v1 ^ v3 ^ v5 ^ v7, (u32)k * 2 + 1);
                }
            }
        }
        if (sum != NULL) {
            sum->add = add;
            sum->rot = rot;
        }
        return;
    }
    for (y = 0; y < h; y++) {
        int ty = sy + y;
        const u16* row = tiles + (ty >> 2) * (tw / 4) * 16 + (ty & 3) * 4;
        u16* out = dst + y * dstStride;

        x = 0;
        if ((sx & 3) == 0 && ((uintptr_t)out & 3) == 0) {
            const u32* in = (const u32*)(row + sx * 4);
            u32* o = (u32*)out;

            for (; x + 4 <= w; x += 4, in += 8, o += 2) {
                o[0] = fb_5a3_to_5551x2(in[0]);
                o[1] = fb_5a3_to_5551x2(in[1]);
            }
        }
        for (; x < w; x++) {
            int tx = sx + x;

            out[x] = (u16)fb_5a3_to_5551x2(row[(tx >> 2) * 16 + (tx & 3)]);
        }
    }
    if (sum != NULL) {
        *sum = fb_sum((const u32*)dst, (u32)(w * h / 2));
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
        if ((sx & 7) == 0 && ((uintptr_t)out & 3) == 0) {
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
        if (((uintptr_t)in & 3) == 0) {
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
        if (((uintptr_t)in & 3) == 0) {
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
    sXferTex.stale = false;
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
/* The texture in sXfer                                                                           */
/* ============================================================================================== */

static void fb_frame_write(void);

static inline bool fb_frame_pending(void) {
    return gGfxFb.dirtyY0 < gGfxFb.dirtyY1 && gGfxFb.dirtyY0 < FB_H && gGfxFb.dirtyY1 > 0;
}

/* Whether sXfer holds the frame as the EFB holds it now (or held it when the off-screen pass started) */
static inline bool fb_xfer_frame_current(void) {
    return sXferTex.kind == XFER_FRAME && sXferTex.key == gGfxFb.frameKey && sXferTex.task == sTask &&
           sXferTex.gen == gGfxFb.frameGen;
}

/*
 * sXfer is about to be reused while the texture gfx_fb_bind_image served may have draws still to come
 * (gfx_gx_image_rect switches the render target or prepares the canvas after the caller bound it): bind the same image
 * from RAM instead, the RAM path's texels. The frame's pending rows go to RAM first (from sXfer during an off-screen
 * pass, else from the EFB, which holds the same pixels: nothing was drawn into the frame since it was copied). An
 * image's RAM is the texture's (written by the copy, or checked) until something writes it, which reuses sXfer first.
 */
static void fb_served_to_ram(void) {
    GfxTexBinding b;

    sServed.on = false;
    if (sServed.frame) {
        if (!fb_xfer_frame_current()) {
            // Drawn into since: the draws that read the copy were issued before that draw
            return;
        }
        if (fb_frame_pending()) {
            fb_frame_write();
        }
    }
    sTexStats.movedToRam++;
    if (!sServed.fromRam(fb_ptr(sServed.key), G_IM_FMT_RGBA, G_IM_SIZ_16b, FB_W, FB_H, FB_W, NULL, false,
                         sServed.linear, sServed.texMap, &b) ||
        !b.valid) {
        if (sLogCount < FB_LOG_LIMIT) {
            sLogCount++;
            gc_log("gfx: fb: background %08X could not be bound from RAM before its draws", (unsigned int)sServed.key);
        }
    }
}

/*
 * sXfer is about to be overwritten (`cpu`: by the CPU, which must wait for queued draws that read it; EFB copies are
 * executed after them). While an off-screen pass covers the frame's corner, sXfer holds the frame's pending rows (see
 * gfx_fb_canvas_begin): they go to RAM first, unless the pass `ends` and the EFB gets the frame back.
 */
static void fb_xfer_release(bool ends, bool cpu) {
    if (sServed.on) {
        fb_served_to_ram();
    }
    if (sXferTex.kind == XFER_FRAME && sCanvas.active && !ends && fb_frame_pending() && fb_xfer_frame_current()) {
        fb_frame_write();
    }
    if (cpu && sXferTex.bound) {
        fb_wait();
    }
    sXferTex.kind = XFER_NONE;
    sXferTex.stale = true;
}

/* The renderer wrote N64 RAM [addr, addr + bytes): textures cached from it are checked again, and an image texture in
 * sXfer read from it is no longer the image */
static void fb_ram_written(u32 addr, u32 bytes) {
    if (sXferTex.kind == XFER_IMAGE &&
        fb_overlap(addr, addr + bytes, sXferTex.key, sXferTex.key + FB_W * FB_H * 2)) {
        sXferTex.kind = XFER_NONE;
    }
    gfx_tex_ram_written(addr, bytes);
}

/* Make sXfer the frame's texture as the EFB holds it now (no off-screen pass): an EFB copy of the whole frame at N64
 * size, box filtered as the readback is, RGB5A3. GPU work only. */
static void fb_frame_capture(void) {
    if (fb_xfer_frame_current()) {
        return;
    }
    fb_xfer_release(false, false);
    fb_copy_begin();
    GX_SetTexCopySrc(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    GX_SetTexCopyDst(FB_W, FB_H, GX_TF_RGB5A3, (GFX_SCALE == 2) ? GX_TRUE : GX_FALSE);
    GX_CopyTex(sXfer, GX_FALSE);
    GX_PixModeSync();
#if GFX_FB_VERIFY
    fb_verify_copy(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT, GFX_SCALE == 2, FB_W, FB_H, "frame copy");
#endif
    fb_copy_end();
    sXferTex.kind = XFER_FRAME;
    sXferTex.key = gGfxFb.frameKey;
    sXferTex.task = sTask;
    sXferTex.gen = gGfxFb.frameGen;
    sTexStats.frameCopies++;
}

/* Whether every row of the frame's RAM is, or would be after gfx_fb_sync_ram, what the EFB holds (pending rows, and
 * rows written to RAM in this task and not drawn since): the frame's texture is then what gfx_tex.c would read */
static bool fb_frame_rows_current(void) {
    int y0 = (gGfxFb.dirtyY0 < 0) ? 0 : gGfxFb.dirtyY0;
    int y1 = (gGfxFb.dirtyY1 > FB_H) ? FB_H : gGfxFb.dirtyY1;
    int i;

    for (i = 0; i < FB_ROW_WORDS; i++) {
        int a = y0 - i * 32, b = y1 - i * 32;
        u32 m = sRowsInRam[i];

        a = (a < 0) ? 0 : a;
        b = (b > 32) ? 32 : b;
        if (a < b) {
            m |= ((b - a == 32) ? 0xFFFFFFFFu : ((1u << (b - a)) - 1)) << a;
        }
        if (i * 32 + 32 > FB_H) {
            m |= 0xFFFFFFFFu << (FB_H - i * 32); // past the last row
        }
        if (m != 0xFFFFFFFFu) {
            return false;
        }
    }
    return true;
}

static void fb_rows_in_ram(int y0, int y1) {
    int y;

    for (y = y0; y < y1; y++) {
        sRowsInRam[y >> 5] |= 1u << (y & 31);
    }
}

/* ============================================================================================== */
/* The frame                                                                                      */
/* ============================================================================================== */

/* Write the frame's pending rows to RAM: EFB rows [2 y0, 2 y1) at half size, or, while an off-screen pass covers the
 * frame's corner, the same rows of the frame's copy in sXfer made when the pass started */
static void fb_frame_write(void) {
    int y0, y1;
    u32 key = gGfxFb.frameKey;
    int tw, sy;

    // Outside a pass the rows go through sXfer: a texture served from it moves to RAM first (the frame's by writing
    // these rows itself, which leaves none pending here)
    if (sServed.on && !sCanvas.active && fb_frame_pending()) {
        fb_xfer_release(false, false);
    }
    y0 = gGfxFb.dirtyY0;
    y1 = gGfxFb.dirtyY1;
    gGfxFb.dirtyY0 = FB_ROWS_NONE;
    gGfxFb.dirtyY1 = 0;
    y0 = (y0 < 0) ? 0 : y0;
    y1 = (y1 > FB_H) ? FB_H : y1;
    if (!sReady || y0 >= y1 || key == 0) {
        return;
    }
    if (gGfxFb.frameWidth != FB_W || !fb_ram_ok(key, FB_W * FB_H * 2)) {
        if (sLogCount < FB_LOG_LIMIT) {
            sLogCount++;
            gc_log("gfx: fb: frame %08X (%u wide) not written to RAM", (unsigned int)key, gGfxFb.frameWidth);
        }
        return;
    }
    if (sCanvas.active) {
        // Nothing draws into the frame during a pass: sXfer holds what the EFB held when it started
        if (!fb_xfer_frame_current()) {
            if (sLogCount < FB_LOG_LIMIT) {
                sLogCount++;
                gc_log("gfx: fb: frame %08X rows %d-%d not written to RAM (no copy)", (unsigned int)key, y0, y1);
            }
            return;
        }
        fb_wait();
        tw = FB_W;
        sy = y0;
        DCInvalidateRange(sXfer + (y0 >> 2) * FB_W * 8, (((y1 + 3) >> 2) - (y0 >> 2)) * FB_W * 8);
    } else {
        fb_xfer_release(false, false);
        fb_copy_begin();
        tw = fb_copy_to_xfer(0, y0 * GFX_SCALE, GFX_EFB_WIDTH, (y1 - y0) * GFX_SCALE, GX_TF_RGB5A3, GFX_SCALE == 2);
#if GFX_FB_VERIFY
        fb_verify_copy(0, y0 * GFX_SCALE, GFX_EFB_WIDTH, (y1 - y0) * GFX_SCALE, GFX_SCALE == 2, tw, y1 - y0,
                       "frame readback");
#endif
        fb_copy_end();
        sy = 0;
        if (y0 == 0 && y1 == FB_H) {
            // The whole frame: also its texture
            sXferTex.kind = XFER_FRAME;
            sXferTex.key = key;
            sXferTex.task = sTask;
            sXferTex.gen = gGfxFb.frameGen;
        }
    }
    fb_rgb5a3_to_rgba16(tw, 0, sy, FB_W, y1 - y0, (u16*)fb_ptr(key) + y0 * FB_W, FB_W, NULL);
    fb_ram_written(key + y0 * FB_W * 2, (y1 - y0) * FB_W * 2);
    fb_rows_in_ram(y0, y1);
    sStats.readbacks++;
    sStats.readbackRows += y1 - y0;
}

/* ============================================================================================== */
/* The canvas                                                                                     */
/* ============================================================================================== */

static inline u32 fb_canvas_bpp(void) {
    return (sCanvas.siz == G_IM_SIZ_8b) ? 1 : 2;
}

/* Write the canvas pixels drawn since the last write to the image's RAM (`ends`: the pass ends with it). When they
 * are a whole RGBA16 image of FB_W x FB_H, the copy is also its texture: every pixel of the RAM then comes from it,
 * with the alpha bit set. (Pixels loaded from RAM may have it clear, which RGB5A3 makes transparent texels.) */
static void fb_canvas_write(bool ends) {
    FbRect d = sCanvas.dirty;
    u32 bpp = fb_canvas_bpp();
    u32 rowBytes = sCanvas.width * bpp;
    int sx0, sy0, sx1, sy1, tw;
    bool whole;
    FbSum sum;
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
    whole = bpp == 2 && sCanvas.width == FB_W && (sCanvas.key & 3) == 0 && d.x0 == 0 && d.y0 == 0 && d.x1 == FB_W &&
            d.y1 == FB_H;
    // EFB copies start and end on even pixels
    sx0 = d.x0 & ~1;
    sy0 = d.y0 & ~1;
    sx1 = (d.x1 + 1) & ~1;
    sy1 = (d.y1 + 1) & ~1;
    fb_xfer_release(ends, false);
    fb_copy_begin();
    tw = fb_copy_to_xfer(sx0, sy0, sx1 - sx0, sy1 - sy0, (bpp == 1) ? GX_CTF_R8 : GX_TF_RGB5A3, false);
#if GFX_FB_VERIFY
    if (bpp == 2) {
        fb_verify_copy(sx0, sy0, sx1 - sx0, sy1 - sy0, false, tw, sy1 - sy0, "off-screen image");
    }
#endif
    fb_copy_end();
    if (bpp == 1) {
        fb_r8_to_i8(tw, d.x0 - sx0, d.y0 - sy0, d.x1 - d.x0, d.y1 - d.y0, (u8*)fb_ptr(first) + d.x0, rowBytes);
    } else {
        fb_rgb5a3_to_rgba16(tw, d.x0 - sx0, d.y0 - sy0, d.x1 - d.x0, d.y1 - d.y0, (u16*)fb_ptr(first) + d.x0,
                            sCanvas.width, whole ? &sum : NULL);
    }
    fb_ram_written(first, (d.y1 - d.y0) * rowBytes);
    if (whole) {
        sXferTex.kind = XFER_IMAGE;
        sXferTex.key = sCanvas.key;
        sXferTex.task = sTask;
        sXferTex.sum = sum;
    }
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
    fb_xfer_release(false, true);
    if (h != FB_H || tw != w) {
        memset(sXfer, 0, FB_W * FB_H * 2);
    }
    if (bpp == 1) {
        fb_i8_to_tiles((const u8*)fb_ptr(sCanvas.key), rowBytes, tw, w, h);
        GX_InitTexObj(&obj, sXfer, tw, (h + 3) & ~3, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    } else {
        fb_rgba16_to_rgb565((const u16*)fb_ptr(sCanvas.key), sCanvas.width, tw, w, h);
        GX_InitTexObj(&obj, sXfer, tw, (h + 3) & ~3, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    }
    DCFlushRange(sXfer, FB_W * FB_H * 2);
    GX_InitTexObjFilterMode(&obj, GX_NEAR, GX_NEAR);
    fb_draw_begin(&obj, NULL);
    fb_quad(0, 0, w, h, (float)w / tw, (float)h / ((h + 3) & ~3));
    fb_draw_end(false);
    sXferTex.bound = true;
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
        fb_canvas_write(false);
    } else {
        // The frame's pending rows stay pending (its RAM is written when something reads it). The canvas is about to
        // cover the frame's corner, so a copy of the frame (GPU work only) keeps them until the pass ends; it is also
        // the texture of backgrounds read from the frame, which off-screen passes usually draw.
#if GFX_FB_AB
        if (sAbRam) {
            fb_frame_write();
        }
#endif
        if (fb_frame_pending() && gGfxFb.frameKey != 0 && gGfxFb.frameWidth == FB_W) {
            fb_frame_capture();
        }
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
        fb_canvas_write(false);
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
    fb_canvas_write(true);
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
    sXferTex.stale = false;
    GX_InitTexObj(out, sSaveColor, FB_W, FB_H, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(out, GX_NEAR, GX_NEAR);
    return true;
}

/* ============================================================================================== */
/* Images as textures                                                                             */
/* ============================================================================================== */

/* Whether sXfer holds the texture of the RGBA16 image at `a`, with RAM that still matches it: written in this task,
 * or checked once per task against the checksum taken when it was written (the CPU may have rewritten it between
 * tasks) */
static bool fb_image_current(u32 a) {
    if (sXferTex.kind != XFER_IMAGE || sXferTex.key != a) {
        return false;
    }
    // Pixels drawn into the canvas for it since go to RAM first (and make sXfer its texture again)
    if (sCanvas.active && gGfxFb.canvasDirty && fb_overlap(sCanvas.key, sCanvas.key + sCanvas.width *
                                                           fb_canvas_bpp() * FB_H, a, a + FB_W * FB_H * 2)) {
        fb_canvas_write(false);
        if (sXferTex.kind != XFER_IMAGE || sXferTex.key != a) {
            return false;
        }
    }
    if (sXferTex.task != sTask) {
        FbSum s = fb_sum((const u32*)fb_ptr(a), FB_IMAGE_WORDS);

        sTexStats.imageChecks++;
        if (s.add != sXferTex.sum.add || s.rot != sXferTex.sum.rot) {
            sTexStats.imageMismatches++;
            sXferTex.kind = XFER_NONE;
            return false;
        }
        sXferTex.task = sTask;
    }
    return true;
}

bool gfx_fb_bind_image(const void* addr, uint16_t width, uint16_t height, uint16_t stride, bool linear, int texMap,
                       GfxBindImageFn fromRam, GfxTexBinding* out) {
    u32 a = fb_key(addr);
    u32 f0 = gGfxFb.frameKey, f1 = f0 + FB_W * FB_H * 2;
    bool frame = false, image = false;
    GXTexObj obj;
    u64 start;

    // The draws of the previous one were issued
    sServed.on = false;
    if (!sReady || fromRam == NULL || a == 0 || (a & 3) != 0 || width != FB_W || height != FB_H || stride != FB_W ||
        !fb_ram_ok(a, FB_W * FB_H * 2)) {
        return false;
    }
#if GFX_FB_AB
    if (sAbRam) {
        return false;
    }
#endif
    start = gettime();
    // Batched draws go to GX first: the texture map is about to change
    gfx_gx_flush();
    if (a == f0 && gGfxFb.frameWidth == FB_W) {
        // The frame: every row of it as the EFB holds it, or as it was when the off-screen pass started
        if (fb_frame_rows_current()) {
            if (!sCanvas.active) {
                fb_frame_capture();
            }
            frame = fb_xfer_frame_current();
        }
    } else if (!(f0 != 0 && fb_frame_pending() && fb_overlap(a, a + FB_W * FB_H * 2, f0, f1))) {
        image = fb_image_current(a);
    }
    if (frame || image) {
        if (sXferTex.stale) {
            GX_InvalidateTexAll();
            sXferTex.stale = false;
        }
        GX_InitTexObj(&obj, sXfer, FB_W, FB_H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
        GX_InitTexObjFilterMode(&obj, linear ? GX_LINEAR : GX_NEAR, linear ? GX_LINEAR : GX_NEAR);
        GX_LoadTexObj(&obj, texMap);
        sXferTex.bound = true;
        memset(out, 0, sizeof(*out));
        out->valid = true;
        out->width = FB_W;
        out->height = FB_H;
        out->sShiftScale = out->tShiftScale = 1.0f;
        out->linear = linear;
        if (frame) {
            sTexStats.frameBgs++;
        } else {
            sTexStats.imageBgs++;
        }
#if GFX_FB_VERIFY
        fb_verify_texture(a, frame);
#endif
        sServed.on = true;
        sServed.frame = frame;
        sServed.key = a;
        sServed.linear = linear;
        sServed.texMap = texMap;
        sServed.fromRam = fromRam;
    }
    sStats.ticks += gettime() - start;
    return frame || image;
}

void gfx_fb_image_done(void) {
    sServed.on = false;
}

/* ============================================================================================== */
/* Reads                                                                                          */
/* ============================================================================================== */

void gfx_fb_sync_ram(const void* addr, uint32_t bytes) {
    u32 a = fb_key(addr);
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
            fb_canvas_write(false);
        }
    }
    sStats.ticks += gettime() - start;
}

/* ============================================================================================== */
/* Depth                                                                                          */
/* ============================================================================================== */

/* EFB depth (24 bits; gfx_internal.h: the N64's screen z / G_MAXZ, shifted and scaled) as an N64 z-buffer value:
 * the RDP's 18-bit z (screen z << 8) compressed to a 3-bit exponent (leading ones) and an 11-bit mantissa, dz 0.
 * Never drawn pixels give the game's clear value (G_MAXFBZ), which the lens flare test looks for. */
static inline u16 fb_n64_depth(u32 d) {
    u32 z, e;

    if (d >= 0xFFFFFF) {
        return 0xFFFC;
    }
    d = gfx_depth24_to_n64(d);
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

/* Drop the data cache's copy of the 32 bytes at p (GX wrote them behind it); nothing there is dirty */
static inline void fb_dcbi(const void* p) {
#ifdef GEKKO
    __asm__ volatile("dcbi 0,%0" : : "r"(p) : "memory");
#else
    (void)p;
#endif
}

/* One EFB tile row (4 rows) as Z24X8 tiles: 64 bytes per 4x4 texels */
#define FB_DEPTH_STRIP_BYTES ((GFX_EFB_WIDTH / 4) * 64)
/* Tile rows per batch of depth copies: what sSaveDepth holds */
#define FB_DEPTH_STRIPS ((FB_W * FB_H * 4) / FB_DEPTH_STRIP_BYTES)

/* The frame's depth into the z-buffer's RAM (320 x 240, 16 bits per pixel), one value per 4x4 pixels: the EFB depth
 * at N64 pixel (1, 1) of each 4x4 block, converted, fills the block. The game reads single pixels and compares them
 * loosely (whether the sun is hidden, whether a light is behind something). Depth is copied without the 2x2 box
 * filter (it would average the 24-bit values channel by channel), and only the EFB tile rows that hold a sample row
 * (a quarter of the EFB), a batch of them at a time. */
static void fb_depth_write(u16* z) {
    const int blockRows = FB_H / 4;
    int b0, n, i, tx;

    for (b0 = 0; b0 < blockRows; b0 += n) {
        n = (blockRows - b0 < FB_DEPTH_STRIPS) ? blockRows - b0 : FB_DEPTH_STRIPS;
        fb_copy_begin();
        GX_SetTexCopyDst(GFX_EFB_WIDTH, 4, GX_TF_Z24X8, GX_FALSE);
        for (i = 0; i < n; i++) {
            GX_SetTexCopySrc(0, (((b0 + i) * 4 + 1) * GFX_SCALE) & ~3, GFX_EFB_WIDTH, 4);
            GX_CopyTex(sSaveDepth + i * FB_DEPTH_STRIP_BYTES, GX_FALSE);
        }
        GX_PixModeSync();
        fb_wait();
        fb_copy_end();

        for (i = 0; i < n; i++) {
            const u8* strip = sSaveDepth + i * FB_DEPTH_STRIP_BYTES;
            int r = (((b0 + i) * 4 + 1) * GFX_SCALE) & 3; /* the sample's row in the tile row */
            u32* row = (u32*)(z + (b0 + i) * 4 * FB_W);

            for (tx = 0; tx < FB_W / 4; tx++, row += 2) {
                int c = (tx * 4 + 1) * GFX_SCALE;
                const u8* tile = strip + (c >> 2) * 64;
                u32 v;

                fb_dcbi(tile);
                fb_dcbi(tile + 32);
                v = fb_n64_depth(fb_z24(tile, r * 4 + (c & 3)));

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
    u32 key = fb_key(zImage);
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
        fb_depth_write((u16*)fb_ptr(key));
        fb_ram_written(key, FB_W * FB_H * 2);
        sStats.ticks += gettime() - start;
#if GFX_FB_VERIFY
        fb_verify_depth((const u16*)fb_ptr(key));
#endif
    }
}

void gfx_fb_take_stats(GfxFbStats* out) {
    *out = sStats;
    memset(&sStats, 0, sizeof(sStats));
    if (sTexStats.frameBgs != 0 || sTexStats.imageBgs != 0 || sTexStats.imageChecks != 0) {
        gc_log("gx: fb: backgrounds drawn from the GPU: %u from the frame (%u copies of it), %u from off-screen "
               "images (%u RAM checks, %u changed); %u bound from RAM before their draws", sTexStats.frameBgs,
               sTexStats.frameCopies, sTexStats.imageBgs, sTexStats.imageChecks, sTexStats.imageMismatches,
               sTexStats.movedToRam);
    }
    memset(&sTexStats, 0, sizeof(sTexStats));
#if GFX_FB_AB
    sAbRam = !sAbRam;
    gc_log("gx: fb: A/B: the next window binds backgrounds %s", sAbRam ? "from RAM" : "from the GPU");
#endif
#if GFX_FB_VERIFY
    if (sVer.copies != 0 || sVer.textures != 0) {
        gc_log("gx: fb: verify: %u copies and %u textures, %u texels compared, %u differ, %u not opaque", sVer.copies,
               sVer.textures, sVer.texels, sVer.mismatches, sVer.nonOpaque);
    }
    memset(&sVer, 0, sizeof(sVer));
#endif
}

#if GFX_FB_VERIFY
/* ============================================================================================== */
/* Verification (test builds)                                                                     */
/* ============================================================================================== */

/* gfx_tex.c's RGB5A3 texel of an RGBA5551 pixel */
static inline u32 fb_ver_rgb5a3(u32 c) {
    if (c & 1) {
        return 0x8000 | (c >> 1);
    }
    return (((c >> 12) & 0xF) << 8) | (((c >> 7) & 0xF) << 4) | ((c >> 2) & 0xF);
}

static void fb_ver_mismatch(const char* what, int x, int y, u32 got, u32 expect) {
    sVer.mismatches++;
    if (sVerLogs < 16) {
        sVerLogs++;
        gc_log("gx: fb: verify: %s texel (%d, %d) is %04X, the RAM path gives %04X", what, x, y, (unsigned int)got,
               (unsigned int)expect);
    }
}

/* sXfer holds an RGB5A3 copy of EFB [x, x + w) x [y, y + h) (`tw` texels per row of tiles, `th` rows) that was just
 * queued: copy the same pixels as RGB565, as the readbacks did before, and compare what the RAM path makes of them */
static void fb_verify_copy(int x, int y, int w, int h, bool half, int tw, int th, const char* what) {
    const u16* t5a3 = (const u16*)sXfer;
    const u16* t565 = (const u16*)sVerify;
    int cols = half ? w / 2 : w;
    int i, j;

    if (sVerify == NULL) {
        return;
    }
    fb_copy_begin();
    GX_SetTexCopySrc(x, y, w, h);
    GX_SetTexCopyDst(tw, th, GX_TF_RGB565, half ? GX_TRUE : GX_FALSE);
    GX_CopyTex(sVerify, GX_FALSE);
    GX_PixModeSync();
    fb_wait();
    DCInvalidateRange(sVerify, tw * ((th + 3) & ~3) * 2);
    DCInvalidateRange(sXfer, tw * ((th + 3) & ~3) * 2);
    for (j = 0; j < th; j++) {
        for (i = 0; i < cols; i++) {
            int t = ((j >> 2) * (tw / 4) + (i >> 2)) * 16 + (j & 3) * 4 + (i & 3);
            u32 v = t565[t];
            u32 expect = fb_ver_rgb5a3((v & 0xFFC0) | ((v & 0x1F) << 1) | 1);

            sVer.texels++;
            if (!(t5a3[t] & 0x8000)) {
                sVer.nonOpaque++;
            }
            if (t5a3[t] != expect) {
                fb_ver_mismatch(what, i, j, t5a3[t], expect);
            }
        }
    }
    sVer.copies++;
}

/* The texture gfx_fb_bind_image serves for image `a` against gfx_tex.c's texels of its RAM (the frame's pending rows
 * are not in RAM: fb_verify_copy compared them when the copy was made) */
static void fb_verify_texture(u32 a, bool frame) {
    const u16* t5a3 = (const u16*)sXfer;
    const u16* ram = (const u16*)fb_ptr(a);
    int x, y;

    fb_wait();
    sXferTex.bound = true; // the draws that read it are still to come
    DCInvalidateRange(sXfer, FB_W * FB_H * 2);
    for (y = 0; y < FB_H; y++) {
        if (frame && y >= gGfxFb.dirtyY0 && y < gGfxFb.dirtyY1) {
            continue;
        }
        for (x = 0; x < FB_W; x++) {
            u32 got = t5a3[((y >> 2) * (FB_W / 4) + (x >> 2)) * 16 + (y & 3) * 4 + (x & 3)];
            u32 expect = fb_ver_rgb5a3(ram[y * FB_W + x]);

            sVer.texels++;
            if (got != expect) {
                fb_ver_mismatch(frame ? "frame texture" : "image texture", x, y, got, expect);
            }
        }
    }
    sVer.textures++;
}

/* The z-buffer's RAM against the way it was written before: whole quarters of the EFB depth copied 1:1 (into
 * sSaveColor, free at task end), each block's sample read from them */
static void fb_verify_depth(const u16* z) {
    const int rows = GFX_EFB_HEIGHT / 4, tilesW = GFX_EFB_WIDTH / 4;
    int q, tx, ty, x, y;

    for (q = 0; q < 4; q++) {
        fb_copy_begin();
        GX_SetTexCopySrc(0, q * rows, GFX_EFB_WIDTH, rows);
        GX_SetTexCopyDst(GFX_EFB_WIDTH, rows, GX_TF_Z24X8, GX_FALSE);
        GX_CopyTex(sSaveColor, GX_FALSE);
        GX_PixModeSync();
        fb_wait();
        fb_copy_end();
        DCInvalidateRange(sSaveColor, GFX_EFB_WIDTH * rows * 4);
        for (ty = q * rows / (4 * GFX_SCALE); ty < (q + 1) * rows / (4 * GFX_SCALE); ty++) {
            int r = (ty * 4 + 1) * GFX_SCALE - q * rows;

            for (tx = 0; tx < FB_W / 4; tx++) {
                int c = (tx * 4 + 1) * GFX_SCALE;
                const u8* tile = sSaveColor + ((r >> 2) * tilesW + (c >> 2)) * 64;
                u32 expect = fb_n64_depth(fb_z24(tile, (r & 3) * 4 + (c & 3)));

                for (y = 0; y < 4; y++) {
                    for (x = 0; x < 4; x++) {
                        u32 got = z[(ty * 4 + y) * FB_W + tx * 4 + x];

                        sVer.texels++;
                        if (got != expect) {
                            fb_ver_mismatch("depth", tx * 4 + x, ty * 4 + y, got, expect);
                        }
                    }
                }
            }
        }
    }
    sVer.copies++;
}
#endif

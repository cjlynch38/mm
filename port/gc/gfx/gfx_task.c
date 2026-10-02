/**
 * Renderer entry points declared in gc_bridge.h (gc_gfx_*): init, the per-task sequence that runs a
 * graphics task's display list through the RSP/RDP interpreters into GX, and presenting frames.
 * GC_RENDERER=0 builds (gc_options.h) keep the M2 text console as the display and skip the tasks.
 *
 * GFX_FB_TEST (compile-time, 0 by default) appends the game's own framebuffer-effect display lists to every
 * frame, built here the way z_vismono.c, z_play.c (motion blur), z_visfbuf.c and PreRender.c build them, with
 * texture rectangles where the game uses S2DEX2 backgrounds, and checks the pixels that reach RAM. Bits:
 *   1  VisMono: the frame in black and white (textures loaded from the frame being drawn)
 *   2  motion blur: last frame's copy blended over the frame, then the frame copied to a work buffer
 *      (off-screen pass); the copy is checked against the frame
 *   4  VisFbuf-style shrink: the frame copied off-screen, cleared, drawn back at 3/4 size
 *   8  coverage-style I8 image: the frame's high bytes drawn into an 8-bit image (checked), shown at half size
 *      in the bottom right corner
 *  16  pause/picto/transition-style capture: the frame copied into the z-buffer's memory as a color image (checked
 *      against the frame); the task must not write its depth over the copy (checked at the next task)
 */
#include <gccore.h>
#include <string.h>
#include "gc_ogc.h"
#include "gc_options.h"
#include "gfx_internal.h"

/* gc_gfx_init() needs this much free MEM1 arena, so the renderer never starves the game:
 *   GX FIFO 256 KiB + texture cache (about 2 MiB, gfx_tex.c) + 3 XFBs of the video mode (614,400 bytes each
 *   at 640x480, 734,720 for PAL's 574 lines) + what the game's shim allocations take after boot (thread
 *   stacks and buffers, measured 1.2 MiB). gfx_fb.c's buffers (750 KiB) are optional and checked by gfx_fb_init. */
#define GFX_MEM_FIXED (256 * 1024 + 2 * 1024 * 1024)
#define GFX_MEM_XFBS 3
#define GFX_MEM_GAME_RESERVE (1536 * 1024)

#ifndef GFX_FB_TEST
#define GFX_FB_TEST 0
#endif

static bool sEnabled;

#if GFX_FB_TEST
static void fbtest_init(void);
static void fbtest_run(void);
#endif

void gc_gfx_init(void) {
    unsigned int arenaFree;
    GXRModeObj* mode;
    unsigned int need;

    if (!GC_RENDERER) {
        gc_log("gfx: renderer off (GC_RENDERER=0): the text console stays the display");
        return;
    }
    arenaFree = (unsigned int)SYS_GetArena1Hi() - (unsigned int)SYS_GetArena1Lo();
    mode = (GXRModeObj*)gc_ogc_video_mode();
    need = GFX_MEM_FIXED + GFX_MEM_XFBS * ((mode != NULL) ? VIDEO_GetFrameBufferSize(mode) : 614400);
    if (arenaFree < need + GFX_MEM_GAME_RESERVE) {
        gc_log("gfx: renderer disabled: %u KB free in MEM1, it needs %u KB plus %u KB kept for the game", arenaFree / 1024,
               need / 1024, GFX_MEM_GAME_RESERVE / 1024);
        return;
    }

    gfx_gx_init();
    if (!gfx_gx_ready()) {
        return;
    }
    gfx_tex_init();
    gfx_tev_init();
    gfx_fb_init();
#if GFX_FB_TEST
    fbtest_init();
#endif
    sEnabled = true;
    gc_log("gfx: renderer ready (%u KB of MEM1 left)",
           ((unsigned int)SYS_GetArena1Hi() - (unsigned int)SYS_GetArena1Lo()) / 1024);
}

int gc_gfx_enabled(void) {
    return sEnabled;
}

void gc_gfx_run_task(unsigned int dlist) {
    if (!sEnabled) {
        return;
    }
    gfx_gx_task_begin();
    gfx_rsp_reset();
    gfx_rdp_reset();
    gfx_tex_frame();
    gfx_rsp_run(dlist);
#if GFX_FB_TEST
    fbtest_run();
#endif
    gfx_gx_task_end();
    gfx_rsp_stats_frame();
}

void gc_gfx_present(const void* framebuffer) {
    if (sEnabled) {
        gfx_gx_present(framebuffer);
    }
}

#if GFX_FB_TEST
/* ============================================================================================== */
/* Framebuffer effect test (see the top of the file)                                              */
/* ============================================================================================== */

#define FBT_W GFX_N64_WIDTH
#define FBT_H GFX_N64_HEIGHT
#define FBT_DL_WORDS 8192
#define FBT_OP(op) ((u32)(op) << 24)

/* Render modes (othermode L) as gbi.h composes them */
#define FBT_CVG_DST_SAVE 0x300
#define FBT_IM_RD 0x40
#define FBT_FORCE_BL 0x4000
#define FBT_GBL_C1(a, b, c, d) (((u32)(a) << 30) | ((u32)(b) << 26) | ((u32)(c) << 22) | ((u32)(d) << 18))
#define FBT_GBL_C2(a, b, c, d) (((u32)(a) << 28) | ((u32)(b) << 24) | ((u32)(c) << 20) | ((u32)(d) << 16))
#define FBT_RM_PASS FBT_GBL_C1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1)
#define FBT_RM_OPA_SURF2 FBT_GBL_C2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1)
#define FBT_RM_CLD_SURF (FBT_IM_RD | FBT_CVG_DST_SAVE | FBT_FORCE_BL | FBT_GBL_C1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define FBT_RM_CLD_SURF2 (FBT_IM_RD | FBT_CVG_DST_SAVE | FBT_FORCE_BL | FBT_GBL_C2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))

static u32* sDl;
static int sDlLen;
static u16* sWork;   /* motion blur: the previous frame */
static u16* sWork2;  /* shrink: this frame */
static u8* sI8;      /* 8-bit image */
static u16* sTlut;   /* VisMono's desaturating IA16 palette */
static u16* sZCopy;  /* z-buffer capture: what the z-buffer's RAM held at the end of the last task */
static const u16* sZPrev;
static u32 sFrames, sCopyErrors, sCopyChecks, sRestoreErrors, sRestoreChecks, sI8Errors, sI8Checks;
static u32 sZErrors, sZChecks, sZKeepErrors, sZKeepChecks;
static bool sHaveWork;

static void fbtest_init(void) {
    int i;

    sDl = gc_mem_alloc(FBT_DL_WORDS * 4, 32);
    sWork = gc_mem_alloc(FBT_W * FBT_H * 2, 64);
    sWork2 = gc_mem_alloc(FBT_W * FBT_H * 2, 64);
    sI8 = gc_mem_alloc(FBT_W * FBT_H, 64);
    sTlut = gc_mem_alloc(256 * 2, 64);
    sZCopy = (GFX_FB_TEST & 16) ? gc_mem_alloc(FBT_W * FBT_H * 2, 64) : sWork2;
    if (sDl == NULL || sWork == NULL || sWork2 == NULL || sI8 == NULL || sTlut == NULL || sZCopy == NULL) {
        gc_log("fbtest: no memory, test off");
        sDl = NULL;
        return;
    }
    // VisMono_DesaturateTLUT: each byte of an RGBA16 pixel as a palette index, high byte -> I, low byte -> A
    for (i = 0; i < 256; i++) {
        u32 hi = (((i >> 3) & 0x1F) * 2 + ((i << 2) & 0x1F) * 4) * 255 / (0x1F * 7);
        u32 lo = (((i >> 6) & 0x1F) * 4 + ((i >> 1) & 0x1F) * 1) * 255 / (0x1F * 7);

        sTlut[i] = (hi << 8) | lo;
    }
    gc_log("fbtest: framebuffer effect test %d (work %p, %p, I8 %p)", GFX_FB_TEST, (void*)sWork, (void*)sWork2,
           (void*)sI8);
}

static void dl(u32 w0, u32 w1) {
    if (sDlLen + 2 <= FBT_DL_WORDS) {
        sDl[sDlLen++] = w0;
        sDl[sDlLen++] = w1;
    }
}

static void dl_othermode(u32 h, u32 l) {
    dl(FBT_OP(G_RDPSETOTHERMODE) | (h & 0xFFFFFF), l);
}

static void dl_combine(u32 a0, u32 b0, u32 c0, u32 d0, u32 aa0, u32 ab0, u32 ac0, u32 ad0, u32 a1, u32 b1, u32 c1,
                       u32 d1, u32 aa1, u32 ab1, u32 ac1, u32 ad1) {
    dl(FBT_OP(G_SETCOMBINE) | ((a0 & 0xF) << 20) | ((c0 & 0x1F) << 15) | ((aa0 & 7) << 12) | ((ac0 & 7) << 9) |
           ((a1 & 0xF) << 5) | (c1 & 0x1F),
       ((b0 & 0xF) << 28) | ((b1 & 0xF) << 24) | ((aa1 & 7) << 21) | ((ac1 & 7) << 18) | ((d0 & 7) << 15) |
           ((ab0 & 7) << 12) | ((ad0 & 7) << 9) | ((d1 & 7) << 6) | ((ab1 & 7) << 3) | (ad1 & 7));
}

static void dl_cimg(u32 fmt, u32 siz, u32 width, const void* img) {
    dl(FBT_OP(G_SETCIMG) | (fmt << 21) | (siz << 19) | (width - 1), (u32)img);
}

static void dl_timg(u32 fmt, u32 siz, u32 width, const void* img) {
    dl(FBT_OP(G_SETTIMG) | (fmt << 21) | (siz << 19) | (width - 1), (u32)img);
}

static void dl_tile(u32 fmt, u32 siz, u32 line, u32 tmem, u32 tile, u32 pal) {
    dl(FBT_OP(G_SETTILE) | (fmt << 21) | (siz << 19) | (line << 9) | tmem,
       (tile << 24) | (pal << 20) | (G_TX_CLAMP << 18) | (G_TX_CLAMP << 8));
}

static void dl_tile_size(u32 tile, u32 uls, u32 ult, u32 lrs, u32 lrt) {
    dl(FBT_OP(G_SETTILESIZE) | (uls << 12) | ult, (tile << 24) | (lrs << 12) | lrt);
}

static void dl_scissor(u32 x0, u32 y0, u32 x1, u32 y1) {
    dl(FBT_OP(G_SETSCISSOR) | ((x0 << 2) << 12) | (y0 << 2), ((x1 << 2) << 12) | (y1 << 2));
}

static void dl_pipesync(void) {
    dl(FBT_OP(G_RDPPIPESYNC), 0);
}

/* gSPTextureRectangle: corners in 10.2, s/t in s10.5, dsdx/dtdy in s5.10 */
static void dl_texrect(u32 ulx, u32 uly, u32 lrx, u32 lry, u32 tile, u32 s, u32 t, u32 dsdx, u32 dtdy) {
    dl(FBT_OP(G_TEXRECT) | (lrx << 12) | lry, (tile << 24) | (ulx << 12) | uly);
    dl(FBT_OP(G_RDPHALF_1), (s << 16) | (t & 0xFFFF));
    dl(FBT_OP(G_RDPHALF_2), (dsdx << 16) | (dtdy & 0xFFFF));
}

/* gDPLoadTextureTile of rows [y0, y1] of a 320-wide image into TMEM 0, render tile 0 sized to them */
static void dl_load_rows(const void* img, u32 fmt, u32 siz, u32 y0, u32 y1) {
    u32 line = ((FBT_W << siz) / 2 + 7) >> 3;

    dl_timg(fmt, siz, FBT_W, img);
    dl_tile(fmt, siz, line, 0, 7, 0);
    dl(FBT_OP(G_RDPLOADSYNC), 0);
    dl(FBT_OP(G_LOADTILE) | (0 << 12) | (y0 << 2), (7 << 24) | (((FBT_W - 1) << 2) << 12) | (y1 << 2));
    dl_pipesync();
    dl_tile(fmt, siz, line, 0, 0, 0);
    dl_tile_size(0, 0, y0 << 2, (FBT_W - 1) << 2, y1 << 2);
}

/* z_vismono.c: VisMono_Draw with VisMono_DesaturateDList */
static void fbtest_vismono(const void* frame) {
    const u32 fragH = (4096 / 2) / (FBT_W * 2);
    u32 y;

    dl_pipesync();
    dl(FBT_OP(G_SETPRIMCOLOR), 0xFFFFFFFF);
    dl(FBT_OP(G_SETENVCOLOR), 0x00000000);
    // gDPLoadTLUT_pal256
    dl_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, sTlut);
    dl(FBT_OP(G_RDPTILESYNC), 0);
    dl(FBT_OP(G_SETTILE) | (256 << 0), 7 << 24);
    dl(FBT_OP(G_RDPLOADSYNC), 0);
    dl(FBT_OP(G_LOADTLUT), (7 << 24) | (255 << 14));
    dl_pipesync();

    dl_othermode(G_AD_DISABLE | G_CD_DISABLE | G_TC_FILT | G_TF_POINT | G_TT_IA16 | G_CYC_2CYCLE | G_PM_1PRIMITIVE,
                 G_AC_NONE | G_ZS_PRIM | FBT_RM_PASS | FBT_RM_CLD_SURF2);
    dl_combine(G_CCMUX_1, G_CCMUX_0, G_CCMUX_TEXEL1_ALPHA, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1,
               G_CCMUX_PRIMITIVE, G_CCMUX_ENVIRONMENT, G_CCMUX_COMBINED, G_CCMUX_ENVIRONMENT, G_ACMUX_0, G_ACMUX_0,
               G_ACMUX_0, G_ACMUX_PRIMITIVE);
    for (y = 0; y <= FBT_H - fragH; y += fragH) {
        const u8* frag = (const u8*)frame + y * FBT_W * 2;

        // gDPLoadTextureBlock(frag, G_IM_FMT_CI, G_IM_SIZ_8b, 640, fragH, ...)
        dl_timg(G_IM_FMT_CI, G_IM_SIZ_16b, 1, frag);
        dl_tile(G_IM_FMT_CI, G_IM_SIZ_16b, 0, 0, 7, 0);
        dl(FBT_OP(G_RDPLOADSYNC), 0);
        dl(FBT_OP(G_LOADBLOCK), (7 << 24) | ((((FBT_W * 2 * fragH + 1) >> 1) - 1) << 12) | ((2048 + 79) / 80));
        dl_pipesync();
        dl_tile(G_IM_FMT_CI, G_IM_SIZ_8b, 80, 0, 0, 0);
        dl_tile_size(0, 0, 0, (640 - 1) << 2, (fragH - 1) << 2);
        // Texel 0 shifted by 2, texel 1 by 1: the high and low byte of each pixel
        dl_tile(G_IM_FMT_CI, G_IM_SIZ_8b, 80, 0, 0, 0);
        dl_tile_size(0, 2 << 2, 0, (640 + 1) << 2, (fragH - 1) << 2);
        dl_tile(G_IM_FMT_CI, G_IM_SIZ_8b, 80, 0, 1, 1);
        dl_tile_size(1, 1 << 2, 0, 640 << 2, (fragH - 1) << 2);
        dl_texrect(0, y << 2, FBT_W << 2, (y + fragH) << 2, 0, 2 << 5, 0, 2 << 10, 1 << 10);
    }
    dl_pipesync();
}

/* Copy the frame to `dst` in COPY mode, 6 rows per load (an off-screen pass, as PreRender/VisFbuf copies do) */
static void fbtest_copy_frame(const void* frame, void* dst) {
    u32 y;

    dl_pipesync();
    dl_cimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, FBT_W, dst);
    dl_scissor(0, 0, FBT_W, FBT_H);
    dl_othermode(G_CYC_COPY | G_TF_POINT | G_TT_NONE, G_AC_NONE);
    for (y = 0; y < FBT_H; y += 6) {
        dl_load_rows(frame, G_IM_FMT_RGBA, G_IM_SIZ_16b, y, y + 5);
        // COPY mode: the lower right corner is inclusive, 4 texels per clock
        dl_texrect(0, y << 2, (FBT_W - 1) << 2, (y + 5) << 2, 0, 0, y << 5, 4 << 10, 1 << 10);
    }
    dl_pipesync();
    dl_cimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, FBT_W, frame);
}

/* Play_DrawMotionBlur, first half (func_80170AE0): last frame's copy over this one with env alpha. The second half
 * is fbtest_copy_frame(frame, sWork). */
static void fbtest_motion_blur_blend(void) {
    u32 y;

    if (!sHaveWork) {
        return;
    }
    dl_pipesync();
    dl_othermode(G_AD_DISABLE | G_CD_DISABLE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_CYC_1CYCLE,
                 G_AC_NONE | G_ZS_PRIM | FBT_RM_CLD_SURF | FBT_RM_CLD_SURF2);
    dl(FBT_OP(G_SETENVCOLOR), 0xFFFFFF00 | 180);
    dl_combine(G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_ENVIRONMENT, G_CCMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0,
               G_ACMUX_ENVIRONMENT, G_CCMUX_TEXEL0, G_CCMUX_0, G_CCMUX_ENVIRONMENT, G_CCMUX_0, G_ACMUX_0, G_ACMUX_0,
               G_ACMUX_0, G_ACMUX_ENVIRONMENT);
    dl_scissor(0, 0, FBT_W, FBT_H);
    for (y = 0; y < FBT_H; y += 6) {
        dl_load_rows(sWork, G_IM_FMT_RGBA, G_IM_SIZ_16b, y, y + 5);
        dl_texrect(0, y << 2, FBT_W << 2, (y + 6) << 2, 0, 0, y << 5, 1 << 10, 1 << 10);
    }
}

/* VisFbuf_ApplyEffects: frame -> work, frame filled, work drawn back at 3/4 size, bilinear */
static void fbtest_shrink(const void* frame) {
    u32 y;

    fbtest_copy_frame(frame, sWork2);
    dl_othermode(G_CYC_FILL, 0);
    dl(FBT_OP(G_SETFILLCOLOR), 0x00010001);
    dl(FBT_OP(G_FILLRECT) | (((FBT_W - 1) << 2) << 12) | ((FBT_H - 1) << 2), 0);
    dl_pipesync();
    dl_othermode(G_AD_DISABLE | G_CD_DISABLE | G_TC_FILT | G_TF_BILERP | G_TT_NONE | G_CYC_1CYCLE,
                 G_AC_NONE | FBT_RM_PASS | FBT_RM_OPA_SURF2);
    dl_combine(G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1, G_CCMUX_0,
               G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1);
    for (y = 0; y < FBT_H; y += 6) {
        // 6 source rows -> 4.5 rows at y * 3/4 + 30, x from 40 to 280; 10.2 corners are exact
        dl_load_rows(sWork2, G_IM_FMT_RGBA, G_IM_SIZ_16b, y, y + 5);
        dl_texrect(40 << 2, 30 * 4 + y * 3, 280 << 2, 30 * 4 + (y + 6) * 3, 0, 0, y << 5, 1365, 1365);
    }
    dl_pipesync();
}

/* PreRender_CoverageRgba16ToI8-style: the frame as IA16 (high byte = I) into an I8 image, then that image shown at
 * half size in the bottom right quarter */
static void fbtest_i8(const void* frame) {
    u32 y;

    dl_pipesync();
    dl_othermode(G_AD_DISABLE | G_CD_DISABLE | G_TC_FILT | G_TF_POINT | G_TT_NONE | G_CYC_1CYCLE,
                 G_AC_NONE | G_ZS_PRIM | FBT_RM_PASS | FBT_RM_OPA_SURF2);
    dl_combine(G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_CCMUX_0,
               G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0);
    dl_cimg(G_IM_FMT_I, G_IM_SIZ_8b, FBT_W, sI8);
    dl_scissor(0, 0, FBT_W, FBT_H);
    for (y = 0; y < FBT_H; y += 6) {
        dl_load_rows(frame, G_IM_FMT_IA, G_IM_SIZ_16b, y, y + 5);
        dl_texrect(0, y << 2, FBT_W << 2, (y + 6) << 2, 0, 0, y << 5, 1 << 10, 1 << 10);
    }
    dl_pipesync();
    dl_cimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, FBT_W, frame);
    dl_combine(G_CCMUX_0, G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1, G_CCMUX_0,
               G_CCMUX_0, G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_0, G_ACMUX_1);
    for (y = 0; y < FBT_H; y += 12) {
        dl_load_rows(sI8, G_IM_FMT_I, G_IM_SIZ_8b, y, y + 11);
        dl_texrect(160 << 2, (120 + y / 2) << 2, FBT_W << 2, (120 + (y + 12) / 2) << 2, 0, 0, y << 5, 2 << 10,
                   2 << 10);
    }
    dl_pipesync();
}

static u32 fbtest_mean(const u16* p) {
    u32 sum = 0;
    int i;

    for (i = 0; i < FBT_W * FBT_H; i++) {
        sum += ((p[i] >> 11) & 0x1F) + ((p[i] >> 6) & 0x1F) + ((p[i] >> 1) & 0x1F);
    }
    return sum * 100 / (FBT_W * FBT_H);
}

/* Run the display list built so far, then start a new one */
static void fbtest_flush_dl(bool endPass) {
    if (endPass) {
        // A FILL into the frame outside an empty scissor: draws nothing, but ends an open off-screen pass as the
        // game's next draw would
        dl_scissor(0, 0, 0, 0);
        dl_othermode(G_CYC_FILL, 0);
        dl(FBT_OP(G_FILLRECT), 0);
    }
    dl(FBT_OP(G_ENDDL), 0);
    gfx_rsp_run((u32)sDl);
    sDlLen = 0;
}

/* The frame's RAM as the EFB holds it now */
static void fbtest_read_frame(const void* frame) {
    gfx_fb_frame_drawn(0, FBT_H);
    gfx_fb_sync_ram(frame, FBT_W * FBT_H * 2);
}

/* Count pixels where a and b differ; log the first few failures */
static void fbtest_compare(const char* what, const u16* a, const u16* b, u32* checks, u32* failures) {
    int i, errors = 0, first = -1;

    for (i = 0; i < FBT_W * FBT_H; i++) {
        if (a[i] != b[i]) {
            if (first < 0) {
                first = i;
            }
            errors++;
        }
    }
    (*checks)++;
    if (errors != 0) {
        (*failures)++;
        if (*failures <= 4) {
            gc_log("fbtest: %s: %d pixels differ, first at (%d, %d): %04X vs %04X", what, errors, first % FBT_W,
                   first / FBT_W, a[first], b[first]);
        }
    }
}

static void fbtest_run(void) {
    const u16* frame = (const u16*)gGfxFb.frameKey;
    int i, errors, first;

    if (sDl == NULL || frame == NULL || gGfxFb.frameWidth != FBT_W || !gfx_fb_ready()) {
        return;
    }
    sDlLen = 0;
    if ((GFX_FB_TEST & 16) && gfx_addr(gGfxRdp.zImageAddr) != NULL) {
        u16* z = gfx_addr(gGfxRdp.zImageAddr);

        // The last task's capture must still be there: its end wrote no depth (the z clear of this task is in the EFB
        // only until this task ends)
        if (sZPrev == z) {
            fbtest_compare("z keep check (z-buffer RAM vs the last capture)", z, sZCopy, &sZKeepChecks,
                           &sZKeepErrors);
        }
        fbtest_read_frame(frame);
        memcpy(sWork2, frame, FBT_W * FBT_H * 2);
        fbtest_copy_frame(frame, z);
        fbtest_flush_dl(true);
        fbtest_compare("z copy check (z-buffer capture vs the frame)", z, sWork2, &sZChecks, &sZErrors);
        memcpy(sZCopy, z, FBT_W * FBT_H * 2);
        sZPrev = z;
        // The tests below inherit the scissor (VisMono sets none): the full screen again, not the flush's empty one
        dl_scissor(0, 0, FBT_W, FBT_H);
    }
    if (GFX_FB_TEST & 1) {
        fbtest_vismono(frame);
    }
    if (GFX_FB_TEST & 2) {
        if ((GFX_FB_TEST & 0xD) == 0) {
            // Checked: the frame after the blend (sWork2 keeps it), the copy of it, the frame after the copy pass
            fbtest_motion_blur_blend();
            fbtest_flush_dl(false);
            fbtest_read_frame(frame);
            memcpy(sWork2, frame, FBT_W * FBT_H * 2);
            if ((sFrames % 100) == 0) {
                gc_log("fbtest: mean RGB (5-bit sum x 100) %u", fbtest_mean(frame));
            }
            fbtest_copy_frame(frame, sWork);
            fbtest_flush_dl(true);
            fbtest_read_frame(frame);
            fbtest_compare("copy check (copy vs frame before the pass)", sWork, sWork2, &sCopyChecks, &sCopyErrors);
            fbtest_compare("restore check (frame after vs before the pass)", frame, sWork2, &sRestoreChecks,
                           &sRestoreErrors);
        } else {
            fbtest_motion_blur_blend();
            fbtest_copy_frame(frame, sWork);
        }
        sHaveWork = true;
    }
    if (GFX_FB_TEST & 4) {
        fbtest_shrink(frame);
    }
    if (GFX_FB_TEST & 8) {
        fbtest_i8(frame);
    }
    fbtest_flush_dl(true);
    sFrames++;

    if ((GFX_FB_TEST & 8) && (GFX_FB_TEST & 7) == 0) {
        // Only the top half and left half of the frame are left as the I8 image saw them
        fbtest_read_frame(frame);
        for (i = 0, errors = 0, first = -1; i < FBT_W * FBT_H; i++) {
            if (i / FBT_W >= FBT_H / 2 && i % FBT_W >= FBT_W / 2) {
                continue;
            }
            if (sI8[i] != (frame[i] >> 8)) {
                if (first < 0) {
                    first = i;
                }
                errors++;
            }
        }
        sI8Checks++;
        sI8Errors += (errors != 0);
        if (errors != 0 && sI8Errors <= 4) {
            gc_log("fbtest: I8 check: %d pixels differ, first at (%d, %d): frame %04X I8 %02X", errors, first % FBT_W,
                   first / FBT_W, frame[first], sI8[first]);
        }
    }
    if ((sFrames % 200) == 100 && gfx_addr(gGfxRdp.zImageAddr) != NULL) {
        // The z-buffer's RAM as the previous task left it (gfx_fb_task_end): never drawn pixels, then exponents
        const u16* z = gfx_addr(gGfxRdp.zImageAddr);
        u32 hist[8] = { 0 }, far = 0;

        for (i = 0; i < FBT_W * FBT_H; i++) {
            if (z[i] == 0xFFFC) {
                far++;
            } else {
                hist[z[i] >> 13]++;
            }
        }
        gc_log("fbtest: z-buffer %08X: %u far, exponents %u %u %u %u %u %u %u %u; center %04X", (unsigned int)z, far,
               hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7], z[FBT_H / 2 * FBT_W + FBT_W / 2]);
    }
    if ((sFrames % 200) == 0) {
        gc_log("fbtest: %u frames; copy checks %u (%u failed), restore checks %u (%u failed), I8 checks %u (%u "
               "failed), z copy checks %u (%u failed), z keep checks %u (%u failed)", sFrames, sCopyChecks,
               sCopyErrors, sRestoreChecks, sRestoreErrors, sI8Checks, sI8Errors, sZChecks, sZErrors, sZKeepChecks,
               sZKeepErrors);
    }
}
#endif

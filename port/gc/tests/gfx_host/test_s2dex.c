/**
 * Host test of the S2DEX2 module (port/gc/gfx/gfx_s2dex.c), driven through gfx_rsp_run() as the game drives it:
 * G_LOAD_UCODE, then S2DEX2 display lists built in fake N64 RAM (gs2dex.h encodings). Checks the backgrounds
 * (G_BG_COPY / G_BG_1CYC: clipping, frame limits, wraps, flips, scales, CI palettes from the TLUT loads, render
 * target images with and without gfx_fb.c), the sprites (render tile and texture rectangle words, the 2D matrix,
 * rotated sprites as triangles and the projection sent again afterwards), G_OBJ_LOADTXTR and its status check,
 * G_SELECT_DL, gSPSetStatus, G_RDPHALF_0 as a texture rectangle half, and the statistics line.
 */
#include <math.h>
#include <string.h>
#include "test.h"

#define CMD(c) ((uint32_t)(c) << 24)

/* S2DEX2 opcodes (gs2dex.h, F3DEX_GBI_2) */
#define OBJ_RECTANGLE 0x01
#define OBJ_SPRITE 0x02
#define SELECT_DL 0x04
#define OBJ_LOADTXTR 0x05
#define OBJ_LDTX_RECT 0x07
#define BG_1CYC 0x09
#define BG_COPY 0x0A
#define OBJ_RENDERMODE 0x0B
#define OBJ_RECTANGLE_R 0xDA
#define OBJ_MOVEMEM 0xDC
#define RDPHALF_0 0xE4

typedef struct {
    uint32_t start, cur;
} Dl;

static Dl dl_new(int cmds) {
    Dl d;

    d.start = d.cur = ram_alloc((uint32_t)cmds * 8);
    return d;
}

static void op(Dl* d, uint32_t w0, uint32_t w1) {
    wr32(d->cur, w0);
    wr32(d->cur + 4, w1);
    d->cur += 8;
}

static uint32_t ucode(const void* sym) {
    return 0x80000000u | (uint32_t)(((uintptr_t)sym - gGfxHostRamBias) & 0x1FFFFFFF);
}

/* gSPLoadUcodeL(gspS2DEX2_fifo) / gSPLoadUcode(F3DZEX2) */
static void load_s2dex(Dl* d) {
    op(d, CMD(G_RDPHALF_1), 0);
    op(d, CMD(G_LOAD_UCODE) | 0x7FF, ucode(gspS2DEX2_fifoTextStart) & 0x1FFFFFFF);
}

static void load_f3d(Dl* d) {
    op(d, CMD(G_RDPHALF_1), 0);
    op(d, CMD(G_LOAD_UCODE) | 0x7FF, ucode(gspF3DZEX2_NoN_PosLight_fifoTextStart));
}

static void end(Dl* d) {
    op(d, CMD(G_ENDDL), 0);
}

static void othermode(Dl* d, uint32_t h, uint32_t l) {
    op(d, CMD(G_RDPSETOTHERMODE) | (h & 0xFFFFFF), l);
}

/* gDPSetScissor in pixels */
static void scissor(Dl* d, int x0, int y0, int x1, int y1) {
    op(d, CMD(G_SETSCISSOR) | ((uint32_t)(x0 * 4) << 12) | (uint32_t)(y0 * 4), ((uint32_t)(x1 * 4) << 12) | (uint32_t)(y1 * 4));
}

static void marker(Dl* d, uint32_t v) {
    op(d, CMD(G_SETPRIMCOLOR), v);
}

static int markers(uint32_t* out, int max) {
    int i, n = 0;

    for (i = 0; i < gRdpCount && i < MAX_RDP; i++) {
        if ((gRdp[i].w0 >> 24) == G_SETPRIMCOLOR && n < max) {
            out[n++] = gRdp[i].w1;
        }
    }
    return n;
}

static void run(Dl* d) {
    mock_reset();
    gfx_rsp_reset();
    gfx_rsp_run(d->start);
}

/* uObjBg / uObjScaleBg (40 bytes) */
typedef struct {
    uint32_t imageX, imageW, frameW, imageY, imageH, frameH; /* u10.5, u10.2 */
    int32_t frameX, frameY;
    uint32_t ptr, fmt, siz, pal, flip, scaleW, scaleH;
} Bg;

static uint32_t put_bg(const Bg* b) {
    uint32_t a = ram_alloc(40);
    uint8_t* p = ram_ptr(a);

    wr16(a + 0, (uint16_t)b->imageX);
    wr16(a + 2, (uint16_t)b->imageW);
    wr16(a + 4, (uint16_t)b->frameX);
    wr16(a + 6, (uint16_t)b->frameW);
    wr16(a + 8, (uint16_t)b->imageY);
    wr16(a + 10, (uint16_t)b->imageH);
    wr16(a + 12, (uint16_t)b->frameY);
    wr16(a + 14, (uint16_t)b->frameH);
    wr32(a + 16, b->ptr);
    wr16(a + 20, 0xFFF4); // G_BGLT_LOADTILE
    p[22] = (uint8_t)b->fmt;
    p[23] = (uint8_t)b->siz;
    wr16(a + 24, (uint16_t)b->pal);
    wr16(a + 26, (uint16_t)b->flip);
    wr16(a + 28, (uint16_t)b->scaleW);
    wr16(a + 30, (uint16_t)b->scaleH);
    return a;
}

/* A w x h background at (x, y) as Prerender_DrawBackground2D builds it: image and frame sizes in 10.2 (the image
 * one quarter pixel larger), scale 1 */
static Bg bg_full(uint32_t ptr, int w, int h, uint32_t fmt, uint32_t siz) {
    Bg b;

    memset(&b, 0, sizeof(b));
    b.imageW = (uint32_t)w * 4 + 1;
    b.imageH = (uint32_t)h * 4 + 1;
    b.frameW = (uint32_t)w * 4;
    b.frameH = (uint32_t)h * 4;
    b.ptr = ptr;
    b.fmt = fmt;
    b.siz = siz;
    b.scaleW = b.scaleH = 1024;
    return b;
}

/* uObjSprite (24 bytes) */
static uint32_t put_sprite(int objX4, int scaleW, int imageW32, int objY4, int scaleH, int imageH32, int stride,
                           int adrs, int fmt, int siz, int pal, int flags) {
    uint32_t a = ram_alloc(24);
    uint8_t* p = ram_ptr(a);

    wr16(a + 0, (uint16_t)objX4);
    wr16(a + 2, (uint16_t)scaleW);
    wr16(a + 4, (uint16_t)imageW32);
    wr16(a + 8, (uint16_t)objY4);
    wr16(a + 10, (uint16_t)scaleH);
    wr16(a + 12, (uint16_t)imageH32);
    wr16(a + 16, (uint16_t)stride);
    wr16(a + 18, (uint16_t)adrs);
    p[20] = (uint8_t)fmt, p[21] = (uint8_t)siz, p[22] = (uint8_t)pal, p[23] = (uint8_t)flags;
    return a;
}

static bool near(double a, double b) {
    return fabs(a - b) < 1e-4;
}

static bool rect_is(int i, double x0, double y0, double x1, double y1, double s, double t, double ds, double dt) {
    const ImageRectRec* r = &gImageRects[i];

    if (i >= gImageRectCount || i >= MAX_IMAGE_RECTS) {
        return false;
    }
    if (!(near(r->ulx, x0) && near(r->uly, y0) && near(r->lrx, x1) && near(r->lry, y1) && near(r->s, s) &&
          near(r->t, t) && near(r->dsdx, ds) && near(r->dtdy, dt))) {
        printf("  rect %d: (%g, %g)-(%g, %g) st (%g, %g) d (%g, %g)\n", i, r->ulx, r->uly, r->lrx, r->lry, r->s, r->t,
               r->dsdx, r->dtdy);
        return false;
    }
    return true;
}

/* ============================================================================================== */
/* Backgrounds                                                                                    */
/* ============================================================================================== */

/* PreRender_CopyImage: a 320x240 RGBA16 image copied 1:1 in copy mode */
static void test_bg_copy(void) {
    uint32_t img = ram_alloc(320 * 240 * 2);
    Bg b = bg_full(img, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);
    Dl d = dl_new(16);

    b.scaleW = b.scaleH = 0; // guS2DInitBg's fields, not scales, in a uObjBg
    load_s2dex(&d);
    othermode(&d, G_CYC_COPY, 0);
    op(&d, CMD(BG_COPY), put_bg(&b));
    load_f3d(&d);
    end(&d);
    run(&d);
    CHECK(gBindCount == 1 && gBinds[0].addr == ram_ptr(img) && gBinds[0].width == 320 && gBinds[0].height == 240 &&
              gBinds[0].stride == 320 && gBinds[0].fmt == G_IM_FMT_RGBA && gBinds[0].siz == G_IM_SIZ_16b &&
              gBinds[0].tlut == NULL && !gBinds[0].linear && gBinds[0].texMap == GX_TEXMAP0,
          "G_BG_COPY binds the whole image (%d binds)", gBindCount);
    CHECK(gFbSyncCount == 1 && gFbSyncAddr == ram_ptr(img) && gFbSyncBytes == 320 * 240 * 2,
          "the image's RAM is synced before binding (%d, %u bytes)", gFbSyncCount, gFbSyncBytes);
    CHECK(gImageRectCount == 1 && rect_is(0, 0, 0, 320, 240, 0, 0, 1, 1), "one 320x240 rectangle (%d)",
          gImageRectCount);
    CHECK(gImageRectCount == 0 || gImageRects[0].b.sOffset == 0.0f, "no half texel shift without filtering");

    // Outside copy mode the RDP reads the same rectangle 4 texels per pixel, one pixel short
    {
        Dl e = dl_new(16);

        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE, 0);
        op(&e, CMD(BG_COPY), put_bg(&b));
        end(&e);
        run(&e);
        // 4 texels per pixel run through 4 image rows per screen row (linear memory), one pixel short
        CHECK(gImageRectCount == 6 && rect_is(0, 0, 0, 80, 239, 0, 0, 4, 1) && rect_is(1, 80, 0, 160, 239, 0, 1, 4, 1) &&
                  gBinds[0].linear == false,
              "G_BG_COPY in 1-cycle mode: 4 texels per pixel (%d rects)", gImageRectCount);
    }
}

/* Clipping to the scissor and the vertical wrap: a frame 10 pixels left of the screen, rows from row 100 */
static void test_bg_copy_clip_wrap(void) {
    uint32_t img = ram_alloc(320 * 240 * 2);
    Bg b = bg_full(img, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);
    Dl d = dl_new(16);

    b.frameX = -10 * 4;
    b.imageY = 100 << 5;
    load_s2dex(&d);
    othermode(&d, G_CYC_COPY, 0);
    op(&d, CMD(BG_COPY), put_bg(&b));
    end(&d);
    run(&d);
    CHECK(gImageRectCount == 2 && rect_is(0, 0, 0, 310, 140, 10, 100, 1, 1) && rect_is(1, 0, 140, 310, 240, 10, 0, 1, 1),
          "clipped on the left, rows wrap at the image's last row (%d rects)", gImageRectCount);

    // Horizontal: a row read past its end continues on the next row (linear memory), then rows wrap
    {
        Dl e = dl_new(16);

        b.frameX = 0;
        b.imageY = 0;
        b.imageX = 300 << 5;
        load_s2dex(&e);
        othermode(&e, G_CYC_COPY, 0);
        op(&e, CMD(BG_COPY), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 3 && rect_is(0, 0, 0, 20, 240, 300, 0, 1, 1) &&
                  rect_is(1, 20, 0, 320, 239, 0, 1, 1, 1) && rect_is(2, 20, 239, 320, 240, 0, 0, 1, 1),
              "past the right edge: the next row, the last one wrapping to row 0 (%d rects)", gImageRectCount);
    }
    // Scissor: a smaller window
    {
        Dl e = dl_new(16);

        b.imageX = 0;
        load_s2dex(&e);
        othermode(&e, G_CYC_COPY, 0);
        scissor(&e, 8, 16, 312, 232);
        op(&e, CMD(BG_COPY), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 1 && rect_is(0, 8, 16, 312, 232, 8, 16, 1, 1), "clipped to the scissor (%d)",
              gImageRectCount);
    }
}

/* G_BG_1CYC: scale, bilinear filtering, the frame limit, flips, the scissor */
static void test_bg_1cyc(void) {
    uint32_t img = ram_alloc(160 * 120 * 2);
    Bg b = bg_full(img, 160, 120, G_IM_FMT_RGBA, G_IM_SIZ_16b);
    Dl d = dl_new(16);

    // Prerender_DrawBackground2D with a scale of 2: frame 320x240, scaleW/H = 1024 / 2
    b.frameW = 320 * 4;
    b.frameH = 240 * 4;
    b.scaleW = b.scaleH = 512;
    load_s2dex(&d);
    othermode(&d, G_CYC_1CYCLE | G_TF_BILERP, 0);
    op(&d, CMD(OBJ_RENDERMODE), 0x0C); // G_OBJRM_ANTIALIAS | G_OBJRM_BILERP
    op(&d, CMD(BG_1CYC), put_bg(&b));
    end(&d);
    run(&d);
    CHECK(gBindCount == 1 && gBinds[0].width == 160 && gBinds[0].height == 120 && gBinds[0].linear,
          "image bound with bilinear filtering");
    CHECK(gImageRectCount == 1 && rect_is(0, 0, 0, 320, 240, 0, 0, 0.5, 0.5), "scaled 2x (%d)", gImageRectCount);
    CHECK(gImageRectCount == 0 || (gImageRects[0].b.sOffset == -0.5f && gImageRects[0].b.tOffset == -0.5f),
          "RDP bilinear sampling: half texel shift");

    // An image exactly frameW / scale wide: the microcode drops the frame's last quarter pixel (VisFbuf's
    // backgrounds, Prerender's images are one quarter pixel wider for this)
    {
        Dl e = dl_new(16);

        b.imageW = 160 * 4;
        b.imageH = 120 * 4;
        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE, 0);
        op(&e, CMD(BG_1CYC), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 1 && rect_is(0, 0, 0, 319, 239, 0, 0, 0.5, 0.5) && !gBinds[0].linear,
              "frame cut to the scaled image (%d)", gImageRectCount);
    }
    // Flipped horizontally: s runs from the right edge
    {
        Dl e = dl_new(16);
        uint32_t img2 = ram_alloc(320 * 240 * 2);
        Bg f = bg_full(img2, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);

        f.flip = 1; // G_BG_FLAG_FLIPS
        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE, 0);
        op(&e, CMD(BG_1CYC), put_bg(&f));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 1 && rect_is(0, 0, 0, 320, 240, 320, 0, -1, 1), "flipped (%d)", gImageRectCount);
    }
    // Scissor and a position: the image's texels follow the clipped edge
    {
        Dl e = dl_new(16);
        uint32_t img2 = ram_alloc(320 * 240 * 2);
        Bg f = bg_full(img2, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);

        f.frameX = 4 * 4;
        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE, 0);
        scissor(&e, 8, 16, 312, 232);
        op(&e, CMD(BG_1CYC), put_bg(&f));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 1 && rect_is(0, 8, 16, 312, 232, 4, 16, 1, 1), "1-cycle clipped (%d)",
              gImageRectCount);
    }
    // Scale 0 is not drawn
    {
        Dl e = dl_new(16);

        b.scaleW = 0;
        load_s2dex(&e);
        op(&e, CMD(BG_1CYC), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gImageRectCount == 0 && gBindCount == 0, "scale 0 not drawn");
    }
}

/* CI backgrounds: the palette comes from the TLUT the game loaded (z_parameter.c's story images: CI8, RGBA16 TLUT) */
static void test_bg_ci(void) {
    uint32_t img = ram_alloc(320 * 240);
    uint32_t pal = ram_alloc(512);
    Bg b = bg_full(img, 320, 240, G_IM_FMT_CI, G_IM_SIZ_8b);
    Dl d = dl_new(24);

    load_s2dex(&d);
    // gDPLoadTLUT(256 entries at TMEM 256)
    op(&d, CMD(G_SETTIMG) | (G_IM_FMT_RGBA << 21) | (G_IM_SIZ_16b << 19), pal);
    op(&d, CMD(G_SETTILE) | 256, (uint32_t)G_TX_LOADTILE << 24);
    op(&d, CMD(G_LOADTLUT), ((uint32_t)G_TX_LOADTILE << 24) | ((255u << 2) << 12));
    othermode(&d, G_CYC_1CYCLE | G_TT_RGBA16, 0);
    op(&d, CMD(BG_1CYC), put_bg(&b));
    end(&d);
    run(&d);
    CHECK(gBindCount == 1 && gBinds[0].fmt == G_IM_FMT_CI && gBinds[0].siz == G_IM_SIZ_8b &&
              gBinds[0].tlut == ram_ptr(pal) && !gBinds[0].tlutIA,
          "CI8 background with the loaded RGBA16 TLUT");
    CHECK(gImageRectCount == 1 && rect_is(0, 0, 0, 320, 240, 0, 0, 1, 1), "drawn (%d)", gImageRectCount);

    // CI4, palette 3 of a 64-entry IA16 TLUT
    {
        Dl e = dl_new(24);
        uint32_t img4 = ram_alloc(64 * 32 / 2);
        Bg c = bg_full(img4, 64, 32, G_IM_FMT_CI, G_IM_SIZ_4b);

        c.pal = 3;
        load_s2dex(&e);
        op(&e, CMD(G_SETTIMG) | (G_IM_FMT_RGBA << 21) | (G_IM_SIZ_16b << 19), pal);
        op(&e, CMD(G_SETTILE) | 256, (uint32_t)G_TX_LOADTILE << 24);
        op(&e, CMD(G_LOADTLUT), ((uint32_t)G_TX_LOADTILE << 24) | ((63u << 2) << 12));
        othermode(&e, G_CYC_1CYCLE | G_TT_IA16, 0);
        op(&e, CMD(BG_1CYC), put_bg(&c));
        end(&e);
        run(&e);
        CHECK(gBindCount == 1 && gBinds[0].siz == G_IM_SIZ_4b && gBinds[0].tlut == ram_ptr(pal + 3 * 16 * 2) &&
                  gBinds[0].tlutIA,
              "CI4 palette 3 of an IA16 TLUT");
    }
    // No TLUT in this task: drawn without one (the TMEM model starts empty each task too)
    {
        Dl e = dl_new(24);
        int logs = gLogCount;

        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE | G_TT_RGBA16, 0);
        op(&e, CMD(BG_1CYC), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gBindCount == 1 && gBinds[0].tlut == NULL && gLogCount >= logs, "no TLUT: bound without a palette");
    }
}

/* Images in render targets (motion blur, pause background, transitions): synced through gfx_fb.c, or skipped
 * without it */
static void test_bg_render_target(void) {
    uint32_t fb = ram_alloc(320 * 240 * 2);
    uint32_t save = ram_alloc(320 * 240 * 2);
    Bg b = bg_full(save, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);
    Dl d = dl_new(24);
    GfxS2dexStats s0, s1;
    int logs;

    // Frame N: the frame is saved into `save` (an off-screen color image); frame N + 1 draws it back
    load_s2dex(&d);
    op(&d, CMD(G_SETCIMG) | (G_IM_SIZ_16b << 19) | 319, fb);
    op(&d, CMD(G_SETCIMG) | (G_IM_SIZ_16b << 19) | 319, save);
    op(&d, CMD(G_SETCIMG) | (G_IM_SIZ_16b << 19) | 319, fb);
    othermode(&d, G_CYC_1CYCLE, 0);
    op(&d, CMD(BG_1CYC), put_bg(&b));
    end(&d);

    gFbReady = false;
    gfx_s2dex_get_stats(&s0);
    logs = gLogCount;
    run(&d);
    gfx_s2dex_get_stats(&s1);
    CHECK(gBindCount == 0 && gImageRectCount == 0 && gFbSyncCount == 0, "without gfx_fb.c: not drawn");
    CHECK(s1.targetBgs == s0.targetBgs + 1 && s1.skipped == s0.skipped + 1 && s1.bgs == s0.bgs, "counted as skipped");
    CHECK(gLogCount > logs && strstr(gLastLog, "render target") != NULL, "logged: %s", gLastLog);

    gFbReady = true;
    run(&d);
    gfx_s2dex_get_stats(&s0);
    CHECK(gFbSyncCount == 1 && gFbSyncAddr == ram_ptr(save) && gBindCount == 1 && gImageRectCount == 1,
          "with gfx_fb.c: synced, bound, drawn");
    CHECK(s0.targetBgs == s1.targetBgs + 1 && s0.bgs == s1.bgs + 1, "counted as drawn from a render target");

    // The z-buffer is a render target too; images elsewhere are not
    {
        Dl e = dl_new(16);
        uint32_t z = ram_alloc(320 * 240 * 2);
        Bg bz = bg_full(z, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);

        gFbReady = false;
        load_s2dex(&e);
        op(&e, CMD(G_SETZIMG), z);
        op(&e, CMD(BG_COPY), put_bg(&bz));
        op(&e, CMD(BG_COPY), put_bg(&b));
        end(&e);
        run(&e);
        CHECK(gBindCount == 0, "z-buffer and an earlier task's color image: still render targets");
        gFbReady = true;
    }
    // Bind failures are counted and logged
    {
        Dl e = dl_new(16);
        uint32_t img = ram_alloc(64 * 64 * 2);
        Bg bi = bg_full(img, 64, 64, G_IM_FMT_RGBA, G_IM_SIZ_16b);

        gBindFail = true;
        gfx_s2dex_get_stats(&s0);
        load_s2dex(&e);
        op(&e, CMD(BG_COPY), put_bg(&bi));
        end(&e);
        run(&e);
        gfx_s2dex_get_stats(&s1);
        CHECK(gBindCount == 1 && gImageRectCount == 0 && s1.skipped == s0.skipped + 1, "bind failure: not drawn");
        gBindFail = false;
    }
    // Images outside RAM are not bound
    {
        Dl e = dl_new(16);
        Bg bo = bg_full(0x817FF000, 320, 240, G_IM_FMT_RGBA, G_IM_SIZ_16b);

        load_s2dex(&e);
        op(&e, CMD(BG_COPY), put_bg(&bo));
        op(&e, CMD(BG_COPY), 0x81F00000); // uObjBg itself outside RAM
        end(&e);
        run(&e);
        CHECK(gBindCount == 0 && gImageRectCount == 0, "images and uObjBg outside RAM: not drawn");
    }
}

/* ============================================================================================== */
/* Objects                                                                                        */
/* ============================================================================================== */

static int find_rdp(uint32_t opcode, int from) {
    int i;

    for (i = from; i < gRdpCount && i < MAX_RDP; i++) {
        if ((gRdp[i].w0 >> 24) == opcode) {
            return i;
        }
    }
    return -1;
}

static void test_obj_rectangle(void) {
    Dl d = dl_new(24);
    uint32_t sp = put_sprite(42, 1024, 32 << 5, 80, 1024, 16 << 5, 8, 0x40, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0);
    uint32_t sub = ram_alloc(8);
    int i;

    load_s2dex(&d);
    othermode(&d, G_CYC_1CYCLE, 0);
    op(&d, CMD(OBJ_RECTANGLE), sp);
    // gSPObjSubMatrix: X 100, Y 50, base scale 2; then gSPObjRectangleR
    wr16(sub, 400);
    wr16(sub + 2, 200);
    wr16(sub + 4, 2048);
    wr16(sub + 6, 2048);
    op(&d, CMD(OBJ_MOVEMEM) | (7 << 16) | 2, sub);
    op(&d, CMD(OBJ_RECTANGLE_R), sp);
    end(&d);
    run(&d);

    i = find_rdp(G_SETTILE, 0);
    CHECK(i >= 0 && gRdp[i].w0 == (CMD(G_SETTILE) | (G_IM_FMT_RGBA << 21) | (G_IM_SIZ_16b << 19) | (8 << 9) | 0x40) &&
              gRdp[i].w1 == (((uint32_t)G_TX_RENDERTILE << 24) | (G_TX_CLAMP << 18) | (G_TX_CLAMP << 8)),
          "render tile from the sprite (%08X %08X)", (i >= 0) ? gRdp[i].w0 : 0, (i >= 0) ? gRdp[i].w1 : 0);
    i = find_rdp(G_SETTILESIZE, 0);
    CHECK(i >= 0 && gRdp[i].w0 == CMD(G_SETTILESIZE) && gRdp[i].w1 == ((31u << 2) << 12 | (15u << 2)),
          "tile size of the image");
    CHECK(gTexrectCount == 2, "two rectangles (%d)", gTexrectCount);
    if (gTexrectCount >= 2) {
        // 10.5 + 32 = 42.5 pixels; 20 + 16 = 36
        CHECK(gTexrects[0].w0 == (CMD(G_TEXRECT) | (170u << 12) | 144) && gTexrects[0].w1 == ((42u << 12) | 80) &&
                  gTexrects[0].half1 == 0 && gTexrects[0].half2 == 0x04000400,
              "G_OBJ_RECTANGLE: %08X %08X %08X %08X", gTexrects[0].w0, gTexrects[0].w1, gTexrects[0].half1,
              gTexrects[0].half2);
        // (10.5 / 2 + 100, 20 / 2 + 50) = (105.25, 60), 16 x 8 pixels, 2 texels per pixel
        CHECK(gTexrects[1].w0 == (CMD(G_TEXRECT) | (485u << 12) | 272) && gTexrects[1].w1 == ((421u << 12) | 240) &&
                  gTexrects[1].half2 == 0x08000800,
              "G_OBJ_RECTANGLE_R through the sub-matrix: %08X %08X %08X", gTexrects[1].w0, gTexrects[1].w1,
              gTexrects[1].half2);
    }

    // Flipped, partly off screen, copy mode: s from the last texel, backwards; 4x dsdx, included lower right
    {
        Dl e = dl_new(16);
        uint32_t f = put_sprite(-8, 1024, 32 << 5, 0, 1024, 16 << 5, 8, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0x01);

        load_s2dex(&e);
        othermode(&e, G_CYC_COPY, 0);
        op(&e, CMD(OBJ_RECTANGLE), f);
        end(&e);
        run(&e);
        CHECK(gTexrectCount == 1 && gTexrects[0].w1 == 0 && gTexrects[0].w0 == (CMD(G_TEXRECT) | (116u << 12) | 60) &&
                  gTexrects[0].half1 == ((uint32_t)(29 * 32) << 16) && (gTexrects[0].half2 >> 16) == 0xF000,
              "flipped, clipped, copy mode: %08X %08X %08X %08X", gTexrects[0].w0, gTexrects[0].w1, gTexrects[0].half1,
              gTexrects[0].half2);
    }
}

/* gSPObjMatrix: A, B, C, D (s15.16), X, Y (s10.2), base scales (u5.10) */
static uint32_t put_obj_mtx(double a, double b, double c, double dd, int x4, int y4) {
    uint32_t m = ram_alloc(24);

    wr32(m, (uint32_t)(int32_t)(a * 65536.0));
    wr32(m + 4, (uint32_t)(int32_t)(b * 65536.0));
    wr32(m + 8, (uint32_t)(int32_t)(c * 65536.0));
    wr32(m + 12, (uint32_t)(int32_t)(dd * 65536.0));
    wr16(m + 16, (uint16_t)x4);
    wr16(m + 18, (uint16_t)y4);
    wr16(m + 20, 1024);
    wr16(m + 22, 1024);
    return m;
}

/* G_OBJ_SPRITE: an axis aligned 2D matrix gives a texture rectangle, a rotation two triangles; the F3DZEX2
 * projection goes to GX again afterwards */
static void test_obj_sprite(void) {
    uint32_t sp = put_sprite(0, 1024, 32 << 5, 0, 1024, 16 << 5, 8, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 0);
    uint32_t vtx = ram_alloc(3 * 16);
    uint32_t pm = ram_alloc(64);
    Dl d = dl_new(32);
    int i, proj;

    // An identity projection and three vertices for F3DZEX2 before and after
    for (i = 0; i < 4; i++) {
        wr16(pm + (uint32_t)(i * 10), 1); // integer part of the diagonal
    }
    wr16(vtx + 16, 10);
    wr16(vtx + 34, 10);
    op(&d, CMD(G_MTX) | (((64 - 1) / 8) << 19) | ((G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH) ^ G_MTX_PUSH), pm);
    op(&d, CMD(G_MTX) | (((64 - 1) / 8) << 19) | ((G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH) ^ G_MTX_PUSH), pm);
    op(&d, CMD(G_VTX) | (3 << 12) | (3 << 1), vtx);
    op(&d, CMD(G_TRI1) | (0 << 16) | (2 << 8) | 4, 0);
    load_s2dex(&d);
    othermode(&d, G_CYC_1CYCLE, 0);
    op(&d, CMD(OBJ_MOVEMEM) | (23 << 16), put_obj_mtx(2.0, 0.0, 0.0, 2.0, 40, 80)); // scale 2 at (10, 20)
    op(&d, CMD(OBJ_SPRITE), sp);
    op(&d, CMD(OBJ_MOVEMEM) | (23 << 16), put_obj_mtx(0.0, -1.0, 1.0, 0.0, 160 * 4, 120 * 4)); // 90 degrees
    op(&d, CMD(OBJ_SPRITE), sp);
    load_f3d(&d);
    op(&d, CMD(G_VTX) | (3 << 12) | (3 << 1), vtx);
    op(&d, CMD(G_TRI1) | (0 << 16) | (2 << 8) | 4, 0);
    end(&d);
    run(&d);

    CHECK(gTexrectCount == 1 && gTexrects[0].w1 == ((40u << 12) | 80) &&
              gTexrects[0].w0 == (CMD(G_TEXRECT) | (296u << 12) | 208) && gTexrects[0].half2 == 0x02000200,
          "axis aligned sprite: a texture rectangle (%d: %08X %08X %08X)", gTexrectCount, gTexrects[0].w0,
          gTexrects[0].w1, gTexrects[0].half2);
    CHECK(gTriCount == 4, "rotated sprite: two triangles between the F3DZEX2 ones (%d)", gTriCount);
    if (gTriCount == 4) {
        const GfxVtx* ul = &gTris[1].v[0];
        const GfxVtx* ur = &gTris[1].v[1];
        const GfxVtx* ll = &gTris[1].v[2];

        // No viewport: 320x240. Object (0, 0) -> screen (160, 120) = NDC (0, 0); object (32, 0) -> (160, 152);
        // object (0, 16) -> (144, 120)
        CHECK(near(ul->x, 0.0) && near(ul->y, 0.0) && near(ur->x, 0.0) && near(ur->y, -32.0 / 120.0) &&
                  near(ll->x, -16.0 / 160.0) && near(ll->y, 0.0) && ul->w == 1.0f,
              "rotated corners in NDC: (%f %f) (%f %f) (%f %f)", ul->x, ul->y, ur->x, ur->y, ll->x, ll->y);
        CHECK(ul->s == 0.0f && ur->s == 32.0f && ll->t == 16.0f && ul->a == 255, "texture coordinates in texels");
        CHECK(gTris[2].v[0].x == ll->x && gTris[2].v[1].x == ur->x, "second triangle LL, UR, LR");
    }
    // Projections: F3DZEX2's, the identity for the sprite, F3DZEX2's again
    proj = gProjCount;
    CHECK(proj == 3 && near(gProj[0][0], 1.0) && near(gProj[3][3], 1.0), "projection sent again after S2DEX2 (%d)",
          proj);
    CHECK(gGfxRsp.geometryMode == G_CLIPPING, "F3DZEX2 geometry mode back");
}

/* G_OBJ_LOADTXTR: loads unless the status word says the texture is already in TMEM */
static void test_obj_loadtxtr(void) {
    uint32_t tx = ram_alloc(24), tile = ram_alloc(24), tlut = ram_alloc(24), sp;
    Dl d = dl_new(32);
    int i, n;

    // TXTRBLOCK: tmem 0x20, 64 words, dxt 0x80, sid 4, flag 1, mask 3
    wr32(tx, 0x00001033);
    wr32(tx + 4, 0x80123400);
    wr16(tx + 8, 0x20);
    wr16(tx + 10, 63);
    wr16(tx + 12, 0x80);
    wr16(tx + 14, 4);
    wr32(tx + 16, 1);
    wr32(tx + 20, 3);
    // TXTRTILE: tmem 0, 32 16-bit texels per row, 8 rows, sid 0 (status 0 already matches flag 0, mask 0: skipped)
    wr32(tile, 0x00FC1034);
    wr32(tile + 4, 0x80123400);
    wr16(tile + 10, (8 << 2) - 1 + 0); // GS_TT_TWIDTH(32, 16b) = (8 words << 2) - 1 = 31
    wr16(tile + 10, 31);
    wr16(tile + 12, (8 << 2) - 1);
    // TLUT: 16 entries at 256 + 32, sid 12, flag 0x10, mask 0x10
    wr32(tlut, 0x00000030);
    wr32(tlut + 4, 0x80124000);
    wr16(tlut + 8, 256 + 32);
    wr16(tlut + 10, 15);
    wr16(tlut + 14, 12);
    wr32(tlut + 16, 0x10);
    wr32(tlut + 20, 0x10);

    load_s2dex(&d);
    op(&d, CMD(OBJ_LOADTXTR), tx);
    op(&d, CMD(OBJ_LOADTXTR), tx);                     // status now matches: skipped
    op(&d, CMD(G_MOVEWORD) | (G_MW_FOG << 16) | 4, 0); // gSPSetStatus(4, 0)
    op(&d, CMD(OBJ_LOADTXTR), tx);                     // loads again
    op(&d, CMD(OBJ_LOADTXTR), tile);                   // flag 0 == status & 0: skipped
    op(&d, CMD(OBJ_LOADTXTR), tlut);
    end(&d);
    run(&d);

    for (i = 0, n = 0; i < gRdpCount; i++) {
        n += (gRdp[i].w0 >> 24) == G_LOADBLOCK;
    }
    CHECK(n == 2, "TXTRBLOCK loaded twice of three (%d)", n);
    i = find_rdp(G_LOADBLOCK, 0);
    CHECK(i >= 2 && gRdp[i - 2].w0 == (CMD(G_SETTIMG) | (G_IM_SIZ_16b << 19)) && gRdp[i - 2].w1 == 0x80123400 &&
              gRdp[i - 1].w0 == (CMD(G_SETTILE) | (G_IM_SIZ_16b << 19) | 0x20) &&
              gRdp[i - 1].w1 == ((uint32_t)G_TX_LOADTILE << 24) && gRdp[i].w0 == CMD(G_LOADBLOCK) &&
              gRdp[i].w1 == (((uint32_t)G_TX_LOADTILE << 24) | (255u << 12) | 0x80),
          "TXTRBLOCK: SETTIMG, SETTILE, LOADBLOCK of 64 words");
    CHECK(find_rdp(G_LOADTILE, 0) < 0, "TXTRTILE skipped by its status");
    i = find_rdp(G_LOADTLUT, 0);
    CHECK(i >= 1 && gRdp[i - 1].w0 == (CMD(G_SETTILE) | (G_IM_SIZ_4b << 19) | (256 + 32)) &&
              gRdp[i].w1 == (((uint32_t)G_TX_LOADTILE << 24) | ((15u << 2) << 12)),
          "TLUT: 16 entries at TMEM 288");

    // The TXTRTILE encoding, with a status that does not match; then G_OBJ_LDTX_RECT draws its sprite
    {
        Dl e = dl_new(16);
        uint32_t txs = ram_alloc(48);

        memcpy(ram_ptr(txs), ram_ptr(tile), 24);
        wr16(txs + 14, 8);
        wr32(txs + 16, 2);
        wr32(txs + 20, 2);
        sp = txs + 24;
        memcpy(ram_ptr(sp), ram_ptr(put_sprite(0, 1024, 32 << 5, 0, 1024, 8 << 5, 8, 0, 0, 2, 0, 0)), 24);
        load_s2dex(&e);
        othermode(&e, G_CYC_1CYCLE, 0);
        op(&e, CMD(OBJ_LDTX_RECT), txs);
        end(&e);
        run(&e);
        i = find_rdp(G_LOADTILE, 0);
        CHECK(i >= 2 && gRdp[i - 2].w0 == (CMD(G_SETTIMG) | (G_IM_SIZ_16b << 19) | 31) &&
                  gRdp[i - 1].w0 == (CMD(G_SETTILE) | (G_IM_SIZ_16b << 19) | (8 << 9)) &&
                  gRdp[i].w1 == (((uint32_t)G_TX_LOADTILE << 24) | ((31u << 2) << 12) | 31),
              "TXTRTILE: 32 texels per row (8 words), 8 rows");
        CHECK(gTexrectCount == 1, "G_OBJ_LDTX_RECT draws its sprite (%d)", gTexrectCount);
    }
}

/* G_SELECT_DL: calls (or branches to) its display list only when the status word changes */
static void test_select_dl(void) {
    Dl sub = dl_new(4), d = dl_new(32), br = dl_new(4);
    uint32_t got[8];
    int n;

    marker(&sub, 0x51);
    end(&sub);
    marker(&br, 0x52);
    end(&br);

    load_s2dex(&d);
    // gSPSelectDL(sub, sid 8, flag 5, mask 0xF)
    op(&d, CMD(RDPHALF_0) | (8 << 16) | (sub.start & 0xFFFF), 5);
    op(&d, CMD(SELECT_DL) | ((uint32_t)G_DL_PUSH << 16) | (sub.start >> 16), 0xF);
    marker(&d, 1);
    op(&d, CMD(RDPHALF_0) | (8 << 16) | (sub.start & 0xFFFF), 5); // same status: not called
    op(&d, CMD(SELECT_DL) | ((uint32_t)G_DL_PUSH << 16) | (sub.start >> 16), 0xF);
    marker(&d, 2);
    op(&d, CMD(G_MOVEWORD) | (G_MW_FOG << 16) | 8, 0); // gSPSetStatus(8, 0)
    op(&d, CMD(RDPHALF_0) | (8 << 16) | (br.start & 0xFFFF), 5); // gSPSelectBranchDL
    op(&d, CMD(SELECT_DL) | ((uint32_t)G_DL_NOPUSH << 16) | (br.start >> 16), 0xF);
    marker(&d, 3); // not reached
    end(&d);
    run(&d);
    n = markers(got, 8);
    CHECK(n == 4 && got[0] == 0x51 && got[1] == 1 && got[2] == 2 && got[3] == 0x52,
          "selected display lists: call, skip, branch (%d: %X %X %X %X)", n, got[0], got[1], got[2], got[3]);
    CHECK(gTexrectCount == 0, "G_RDPHALF_0 of G_SELECT_DL draws nothing");

    // G_RDPHALF_0 with G_RDPHALF_1 and G_RDPHALF_2: a texture rectangle, as in F3DEX2
    {
        Dl e = dl_new(16);

        load_s2dex(&e);
        op(&e, CMD(RDPHALF_0) | (100 << 12) | 80, (2u << 24) | (20 << 12) | 10);
        op(&e, CMD(G_RDPHALF_1), 0x00200040);
        op(&e, CMD(G_RDPHALF_2), 0x04000400);
        end(&e);
        run(&e);
        CHECK(gTexrectCount == 1 && gTexrects[0].w0 == (CMD(G_TEXRECT) | (100 << 12) | 80) &&
                  gTexrects[0].half1 == 0x00200040 && gTexrects[0].half2 == 0x04000400,
              "G_RDPHALF_0 + G_RDPHALF_1/2: texture rectangle (%d)", gTexrectCount);
    }
}

/* The statistics line shows each interval's S2DEX2 activity */
static void test_s2dex_stats(void) {
    uint32_t img = ram_alloc(32 * 32 * 2);
    Bg b = bg_full(img, 32, 32, G_IM_FMT_RGBA, G_IM_SIZ_16b);
    Dl d = dl_new(16);
    int i, before;

    // Drain what earlier tests counted
    before = gLogCount;
    for (i = 0; i < 100000 && gLogCount == before; i++) {
        gfx_rsp_stats_frame();
    }
    load_s2dex(&d);
    othermode(&d, G_CYC_COPY, 0);
    op(&d, CMD(BG_COPY), put_bg(&b));
    op(&d, CMD(BG_COPY), put_bg(&b));
    end(&d);
    run(&d);
    before = gLogCount;
    for (i = 0; i < 100000 && gLogCount == before; i++) {
        gfx_rsp_stats_frame();
    }
    CHECK(strstr(gLastLog, "gfx_s2dex: 2 backgrounds in 2 rects (0 from render targets), 0 not drawn, 0 sprites") !=
              NULL,
          "S2DEX2 statistics: %s", gLastLog);
}

void test_s2dex(void) {
    static void (*const sTests[])(void) = {
        test_bg_copy,       test_bg_copy_clip_wrap, test_bg_1cyc,   test_bg_ci,       test_obj_rectangle,
        test_obj_sprite,    test_obj_loadtxtr,      test_select_dl, test_s2dex_stats,
        // Last: the render targets it records stay (as across tasks) and would cover the next tests' images
        test_bg_render_target,
    };
    size_t i;

    gFbReady = true;
    for (i = 0; i < sizeof(sTests) / sizeof(sTests[0]); i++) {
        ram_reset(); // the images take most of the test RAM
        sTests[i]();
    }
    gFbReady = false;
    ram_reset();
}

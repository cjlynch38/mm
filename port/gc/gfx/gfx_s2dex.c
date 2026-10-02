/**
 * S2DEX2 (gspS2DEX2_fifo), the 2D microcode MM loads with G_LOAD_UCODE around its background draws:
 * Prerender_DrawBackground2D (motion blur, the pause background and picture box copies, Grandma's story),
 * z_visfbuf.c (screen shrink, color interpolation) and the wipe5 transition. MM uses G_BG_COPY, G_BG_1CYC and
 * G_OBJ_RENDERMODE; the object commands (G_OBJ_RECTANGLE, _R, G_OBJ_SPRITE, G_OBJ_LOADTXTR, G_OBJ_LDTX_*,
 * G_OBJ_MOVEMEM), G_SELECT_DL and the status words follow gs2dex.h for completeness. gfx_rsp.c runs the commands
 * S2DEX2 shares with F3DEX2 (display lists, other modes, segments, RDP commands).
 *
 * Backgrounds (uObjBg): the geometry is the microcode's (clipping to the scissor, in 1-cycle mode a frame no larger
 * than the scaled image, G_BG_FLAG_FLIPS), but the image is drawn from RAM as one texture (gfx_tex_bind_image,
 * gfx_gx_image_rect) instead of strips loaded into TMEM, so strips never leave seams. The microcode reads the
 * image as linear memory: a row read past its end continues on the next row, and rows past the last one wrap to
 * the first; each wrapped part is one more rectangle. Every image goes through gfx_fb_sync_ram first, since most
 * of MM's are framebuffers; without gfx_fb.c, images in N64 render targets (recorded from G_SETCIMG / G_SETZIMG)
 * are skipped, their RAM never having received what was drawn there. CI images take their palette from the TLUT
 * load (G_LOADTLUT) that holds it, as the RDP reads it from TMEM.
 *
 * Sprites (uObjSprite) are drawn as the microcode draws them, with RDP commands: the render tile from the sprite
 * (its texels are in TMEM, loaded by G_OBJ_LOADTXTR or by the game) and a texture rectangle. A G_OBJ_SPRITE whose
 * 2D matrix rotates or shears becomes two triangles (NDC coordinates through an identity projection; gfx_rsp.c
 * sends its own projection again once F3DZEX2 is back).
 */
#include <string.h>
#include "gfx_internal.h"

/* S2DEX2 opcodes and constants (include/PR/gs2dex.h, F3DEX_GBI_2) */
#define S2D_OBJ_RECTANGLE 0x01
#define S2D_OBJ_SPRITE 0x02
#define S2D_OBJ_LOADTXTR 0x05
#define S2D_OBJ_LDTX_SPRITE 0x06
#define S2D_OBJ_LDTX_RECT 0x07
#define S2D_OBJ_LDTX_RECT_R 0x08
#define S2D_BG_1CYC 0x09
#define S2D_BG_COPY 0x0A
#define S2D_OBJ_RENDERMODE 0x0B
#define S2D_OBJ_RECTANGLE_R 0xDA
#define S2D_OBJ_MOVEMEM 0xDC

#define S2D_MV_MATRIX 0
#define S2D_MV_SUBMATRIX 2

#define OBJRM_NOTXCLAMP 0x01
#define BG_FLAG_FLIPS 0x01
#define OBJ_FLAG_FLIPS 0x01
#define OBJ_FLAG_FLIPT 0x10

#define OBJLT_TXTRBLOCK 0x00001033u
#define OBJLT_TXTRTILE 0x00FC1034u
#define OBJLT_TLUT 0x00000030u

#define UOBJTXTR_SIZE 24 /* uObjTxtr, followed by the uObjSprite of the G_OBJ_LDTX_* forms */

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x01800000u /* GameCube MEM1 */

#define MAX_RECTS 64 /* rectangles per background (wraps of a frame larger than its image) */
#define TARGETS 16   /* render targets remembered */
#define TLUTS 8      /* TLUT loads remembered */
#define LOG_LIMIT 4

typedef struct {
    float a, b, c, d;               /* s15.16 */
    float x, y;                     /* s10.2, in pixels */
    float baseScaleX, baseScaleY;   /* u5.10 */
} S2dMtx;

typedef struct {
    uint32_t start, end; /* KSEG0 */
    uint32_t stamp;
} S2dTarget;

typedef struct {
    uint32_t tmem;  /* TMEM word of the first entry (256 + palette index) */
    uint32_t count; /* entries */
    uint32_t addr;  /* KSEG0 address of the first entry */
    uint32_t stamp;
} S2dTlut;

typedef struct {
    uint32_t imageX, imageY; /* u10.5 */
    uint32_t imageW, imageH, frameW, frameH; /* u10.2 */
    int32_t frameX, frameY;  /* s10.2 */
    uint32_t imagePtr;
    uint32_t fmt, siz, pal, flip;
    uint32_t scaleW, scaleH; /* u5.10 (uObjScaleBg only) */
} S2dBg;

typedef struct {
    float objX, objY;          /* pixels */
    float scaleW, scaleH;      /* texels per pixel (u5.10) */
    float imageW, imageH;      /* texels (u10.5) */
    uint32_t stride, adrs;     /* TMEM words per row, TMEM word of texel (0, 0) */
    uint32_t fmt, siz, pal, flags;
} S2dSprite;

typedef struct {
    GfxTexBinding b;
    double width, rows; /* the image's texels per row and rows: where it wraps */
} S2dImage;

enum {
    LOG_BAD_BG,
    LOG_BG_EMPTY,
    LOG_BG_SCALE,
    LOG_BG_RAM,
    LOG_BG_BIND,
    LOG_BG_TLUT,
    LOG_BG_RECTS,
    LOG_BAD_OBJ,
    LOG_TXTR,
    LOG_SID,
    LOG_MOVEMEM,
    LOG_COUNT
};

static struct {
    uint32_t renderMode; /* G_OBJ_RENDERMODE */
    uint32_t status[4];  /* gSPSetStatus words, sid 0, 4, 8, 12 */
    S2dMtx mtx;          /* G_OBJ_MOVEMEM */
} sS2d;

static S2dTarget sTargets[TARGETS];
static S2dTlut sTluts[TLUTS];
static uint32_t sStamp;
static GfxS2dexStats sStats;
static uint8_t sLogged[LOG_COUNT];
static int sTargetLogs;

#define LOG_ONCE(id, ...)          \
    do {                           \
        if (!sLogged[id]) {        \
            sLogged[id] = 1;       \
            gc_log(__VA_ARGS__);   \
        }                          \
    } while (0)

/* ============================================================================================== */
/* Helpers                                                                                        */
/* ============================================================================================== */

/* N64 structures are big endian, the GameCube's own byte order */
static inline uint32_t rd16(const uint8_t* p) {
    return ((uint32_t)p[0] << 8) | p[1];
}

static inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Segmented or KSEG address -> KSEG0 address (0 stays 0), as gfx_rsp.c resolves display list addresses */
static uint32_t s2d_resolve(uint32_t addr) {
    if (addr == 0) {
        return 0;
    }
    if (addr & 0x80000000) {
        return RAM_BASE | (addr & 0x1FFFFFFF);
    }
    return RAM_BASE | ((gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x1FFFFFFF);
}

static bool s2d_in_ram(uint32_t a, uint32_t size) {
    return a != 0 && size <= RAM_SIZE && a - RAM_BASE <= RAM_SIZE - size;
}

/* CPU pointer to `size` bytes at a display list address, or NULL if they are not all in RAM */
static const uint8_t* s2d_ptr(uint32_t addr, uint32_t size) {
    uint32_t a = s2d_resolve(addr);

    return s2d_in_ram(a, size) ? (const uint8_t*)gfx_addr(a) : NULL;
}

static inline bool s2d_copy_mode(void) {
    return (gGfxRdp.otherModeH & (3u << G_MDSFT_CYCLETYPE)) == G_CYC_COPY;
}

static inline int32_t clamp_s16(float v) {
    return (v < -32768.0f) ? -32768 : (v > 32767.0f) ? 32767 : (int32_t)v;
}

/* ============================================================================================== */
/* RDP commands, encoded as the microcode sends them                                              */
/* ============================================================================================== */

static void s2d_settimg(uint32_t fmt, uint32_t siz, uint32_t width, uint32_t addr) {
    gfx_rdp_command(((uint32_t)G_SETTIMG << 24) | ((fmt & 7) << 21) | ((siz & 3) << 19) | ((width - 1) & 0xFFF),
                    addr);
}

static void s2d_settile(uint32_t tile, uint32_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint32_t pal,
                        uint32_t cm) {
    gfx_rdp_command(((uint32_t)G_SETTILE << 24) | ((fmt & 7) << 21) | ((siz & 3) << 19) | ((line & 0x1FF) << 9) |
                        (tmem & 0x1FF),
                    ((tile & 7) << 24) | ((pal & 0xF) << 20) | ((cm & 3) << 18) | ((cm & 3) << 8));
}

/* G_SETTILESIZE, G_LOADTILE, G_LOADBLOCK, G_LOADTLUT */
static void s2d_tile_cmd(uint32_t op, uint32_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    gfx_rdp_command((op << 24) | ((uls & 0xFFF) << 12) | (ult & 0xFFF),
                    ((tile & 7) << 24) | ((lrs & 0xFFF) << 12) | (lrt & 0xFFF));
}

/* ============================================================================================== */
/* Render targets and TLUT loads seen by the RDP                                                  */
/* ============================================================================================== */

void gfx_s2dex_note_image(uint32_t w0, uint32_t w1) {
    uint32_t start = s2d_resolve(w1);
    uint32_t width, siz, rows, end;
    int i, slot = 0;

    if (start == 0) {
        return;
    }
    if ((w0 >> 24) == G_SETCIMG) {
        width = (w0 & 0x3FF) + 1;
        siz = (w0 >> 19) & 3;
    } else {
        width = (gGfxRdp.colorImageWidth != 0) ? gGfxRdp.colorImageWidth : GFX_N64_WIDTH;
        siz = G_IM_SIZ_16b;
    }
    /* The height is not in the command: 4:3 plus a margin (MM's 320x240 frames, the 576x454 hi-res ones) */
    rows = width * 3 / 4 + width / 16;
    end = start + ((width * (4u << siz) + 7) / 8) * rows;
    sStamp++;
    for (i = 0; i < TARGETS; i++) {
        if (sTargets[i].start == start) {
            slot = i;
            break;
        }
        if (sTargets[i].stamp < sTargets[slot].stamp) {
            slot = i;
        }
    }
    sTargets[slot].start = start;
    if (sTargets[slot].end < end || i == TARGETS) {
        sTargets[slot].end = end;
    }
    sTargets[slot].stamp = sStamp;
}

static bool s2d_is_target(uint32_t start, uint32_t bytes) {
    int i;

    for (i = 0; i < TARGETS; i++) {
        if (sTargets[i].stamp != 0 && start < sTargets[i].end && sTargets[i].start < start + bytes) {
            return true;
        }
    }
    return false;
}

void gfx_s2dex_note_tlut(uint32_t w0, uint32_t w1) {
    const GfxTile* t = &gGfxRdp.tiles[(w1 >> 24) & 7];
    uint32_t sl = (w0 >> 14) & 0x3FF, tl = (w0 >> 2) & 0x3FF, sh = (w1 >> 14) & 0x3FF;
    int i, slot = 0;

    if (gGfxRdp.texImageAddr == 0 || sh < sl) {
        return;
    }
    for (i = 1; i < TLUTS; i++) {
        if (sTluts[i].stamp < sTluts[slot].stamp) {
            slot = i;
        }
    }
    sTluts[slot].tmem = t->tmem;
    sTluts[slot].count = sh - sl + 1;
    sTluts[slot].addr = gGfxRdp.texImageAddr + (tl * gGfxRdp.texImageWidth + sl) * 2;
    sTluts[slot].stamp = ++sStamp;
}

/* RAM address of TLUT entries [first, first + count), from the newest load that holds them all */
static bool s2d_find_tlut(uint32_t first, uint32_t count, uint32_t* addr) {
    uint32_t w = 256 + first;
    int i, best = -1;

    for (i = 0; i < TLUTS; i++) {
        const S2dTlut* l = &sTluts[i];

        if (l->stamp != 0 && l->tmem <= w && w + count <= l->tmem + l->count &&
            (best < 0 || l->stamp > sTluts[best].stamp)) {
            best = i;
        }
    }
    if (best < 0) {
        return false;
    }
    *addr = sTluts[best].addr + (w - sTluts[best].tmem) * 2;
    return true;
}

/* ============================================================================================== */
/* Backgrounds                                                                                    */
/* ============================================================================================== */

static bool s2d_read_bg(uint32_t addr, S2dBg* bg) {
    const uint8_t* p = s2d_ptr(addr, 40);

    if (p == NULL) {
        LOG_ONCE(LOG_BAD_BG, "gfx_s2dex: uObjBg at bad address %08X (logged once)", (unsigned int)addr);
        return false;
    }
    bg->imageX = rd16(p + 0);
    bg->imageW = rd16(p + 2);
    bg->frameX = (int16_t)rd16(p + 4);
    bg->frameW = rd16(p + 6);
    bg->imageY = rd16(p + 8);
    bg->imageH = rd16(p + 10);
    bg->frameY = (int16_t)rd16(p + 12);
    bg->frameH = rd16(p + 14);
    bg->imagePtr = rd32(p + 16);
    /* imageLoad (LOADBLOCK or LOADTILE) and the tmem fields only change how the microcode fills TMEM */
    bg->fmt = p[22] & 7;
    bg->siz = p[23] & 3;
    bg->pal = rd16(p + 24) & 0xF;
    bg->flip = rd16(p + 26);
    bg->scaleW = rd16(p + 28);
    bg->scaleH = rd16(p + 30);
    return true;
}

/* Bind a background's image (TEXEL0, GX_TEXMAP0) as the RDP would read it: CI texels through the TLUT the game
 * loaded when the other mode enables one. False if it cannot be drawn. */
static bool s2d_bg_bind(const S2dBg* bg, S2dImage* im) {
    uint32_t width = bg->imageW >> 2, height = bg->imageH >> 2;
    uint32_t omH = gGfxRdp.otherModeH;
    uint32_t tt = omH & (3u << G_MDSFT_TEXTLUT);
    uint32_t addr = s2d_resolve(bg->imagePtr);
    uint32_t bytes, tlutAddr;
    const void* tlut = NULL;
    const void* p;
    bool linear;

    if (width == 0 || height == 0 || width > 1024 || height > 1024) {
        LOG_ONCE(LOG_BG_EMPTY, "gfx_s2dex: background image of %ux%u texels not drawn (logged once)",
                 (unsigned int)width, (unsigned int)height);
        return false;
    }
    bytes = ((width * (4u << bg->siz) + 7) / 8) * height;
    if (!s2d_in_ram(addr, bytes)) {
        LOG_ONCE(LOG_BG_RAM, "gfx_s2dex: background image %08X (%u bytes) is outside RAM (logged once)",
                 (unsigned int)bg->imagePtr, (unsigned int)bytes);
        return false;
    }
    if (s2d_is_target(addr, bytes)) {
        sStats.targetBgs++;
        if (!gfx_fb_ready()) {
            if (sTargetLogs < LOG_LIMIT) {
                sTargetLogs++;
                gc_log("gfx_s2dex: background from render target %08X skipped: no framebuffer readback (first %d "
                       "logged)", (unsigned int)addr, LOG_LIMIT);
            }
            return false;
        }
    }
    p = gfx_addr(addr);
    gfx_fb_sync_ram(p, bytes);

    if (tt != G_TT_NONE && bg->siz <= G_IM_SIZ_8b) {
        /* CI4 reads the 16 entries of its palette, CI8 all 256 */
        uint32_t first = (bg->siz == G_IM_SIZ_4b) ? bg->pal * 16 : 0;
        uint32_t count = (bg->siz == G_IM_SIZ_4b) ? 16 : 256;

        if (s2d_find_tlut(first, count, &tlutAddr) && s2d_in_ram(tlutAddr, count * 2)) {
            tlut = gfx_addr(tlutAddr);
        } else {
            LOG_ONCE(LOG_BG_TLUT, "gfx_s2dex: TLUT of a CI background not found, drawn as intensity (logged once)");
        }
    }
    linear = (omH & (3u << G_MDSFT_TEXTFILT)) != G_TF_POINT && !s2d_copy_mode();
    if (!gfx_tex_bind_image(p, (uint8_t)bg->fmt, (uint8_t)bg->siz, (uint16_t)width, (uint16_t)height,
                            (uint16_t)width, tlut, tt == G_TT_IA16, linear, GX_TEXMAP0, &im->b) ||
        !im->b.valid) {
        LOG_ONCE(LOG_BG_BIND, "gfx_s2dex: background image %08X (%ux%u, format %u/%u) could not be bound (logged once)",
                 (unsigned int)addr, (unsigned int)width, (unsigned int)height, (unsigned int)bg->fmt,
                 (unsigned int)bg->siz);
        return false;
    }
    if (linear) {
        /* The RDP's bilinear filter samples texel s at s, GX at s + 0.5 (as gfx_tex_bind does for tiles) */
        im->b.sOffset = -0.5f;
        im->b.tOffset = -0.5f;
    }
    im->width = width;
    im->rows = height;
    return true;
}

/* floor / ceil without libm calls (the values stay far below 2^53) */
static inline double s2d_floor(double v) {
    double t = (double)(int64_t)v;

    return (t > v) ? t - 1.0 : t;
}

static inline double s2d_ceil(double v) {
    double t = (double)(int64_t)v;

    return (t < v) ? t + 1.0 : t;
}

/* Rows [y0, y1) of screen x [x0, x1) (N64 pixels) of a background whose texture coordinates at (x0, y0) are
 * (s0, t0), advancing ds and dt per pixel. As the microcode reads memory, s past the end of a row continues on
 * the next row, and t past the last row wraps to the first: each part is a rectangle of its own, cut where the
 * RDP's samples (pixel corners) cross the edge. */
static void s2d_bg_draw(const S2dImage* im, float x0, float x1, int y0, int y1, double s0, double t0, double ds,
                        double dt) {
    const double w = im->width, h = im->rows, eps = 1.0 / 4096.0;
    double sEnd = s0 + (x1 - x0) * ds;
    double lo = (ds >= 0.0) ? s0 : sEnd, hi = (ds >= 0.0) ? sEnd : s0;
    double k, k0 = s2d_floor(lo / w), k1 = s2d_floor((hi - eps) / w);
    int rects = 0;

    k1 = (k1 < k0) ? k0 : (k1 > k0 + MAX_RECTS) ? k0 + MAX_RECTS : k1;
    for (k = k0; k <= k1; k++) {
        double xa = x0, xb = x1, sa, ta, m, m0, m1;

        if (ds > 0.0) {
            xa = x0 + (k * w - s0) / ds;
            xb = x0 + ((k + 1.0) * w - s0) / ds;
        } else if (ds < 0.0) {
            xa = x0 + ((k + 1.0) * w - s0) / ds;
            xb = x0 + (k * w - s0) / ds;
        }
        xa = (xa < x0) ? x0 : xa;
        xb = (xb > x1) ? x1 : xb;
        if (xb <= xa) {
            continue;
        }
        /* This part reads rows k further on */
        sa = s0 + (xa - x0) * ds - k * w;
        ta = t0 + k;
        m0 = s2d_floor(ta / h);
        m1 = s2d_floor((ta + (y1 - y0) * dt - eps) / h);
        m1 = (m1 < m0) ? m0 : (m1 > m0 + MAX_RECTS) ? m0 + MAX_RECTS : m1;
        for (m = m0; m <= m1; m++) {
            /* Rows whose t (at the row's top) is in [m h, (m + 1) h) */
            double ya = y0 + s2d_ceil((m * h - ta) / dt);
            double yb = y0 + s2d_ceil(((m + 1.0) * h - ta) / dt);

            ya = (ya < y0) ? y0 : ya;
            yb = (yb > y1) ? y1 : yb;
            if (yb <= ya) {
                continue;
            }
            if (rects == MAX_RECTS) {
                LOG_ONCE(LOG_BG_RECTS, "gfx_s2dex: background wraps more than %d times, cut (logged once)", MAX_RECTS);
                return;
            }
            rects++;
            gfx_gx_image_rect((float)xa, (float)ya, (float)xb, (float)yb, &im->b, (float)sa,
                              (float)(ta + (ya - y0) * dt - m * h), (float)ds, (float)dt);
        }
    }
    sStats.rects += (uint32_t)rects;
}

/* G_BG_COPY (uObjBg): one texel per pixel. The microcode clips the frame to the scissor and sends texture
 * rectangles with dsdx 4.0 and an included lower right edge, which only copy mode draws 1:1; in other cycle types
 * the RDP draws them 4 texels per pixel, one pixel short. */
static void s2d_bg_copy(const S2dBg* bg) {
    int32_t ulx = gGfxRdp.scissorUlx, uly = gGfxRdp.scissorUly, lrx = gGfxRdp.scissorLrx, lry = gGfxRdp.scissorLry;
    bool flip = (bg->flip & BG_FLAG_FLIPS) != 0;
    int32_t aw, bw, cw, ah, bh, ch, sx, sy, ex, ey, imgX, imgY;
    float x0, x1;
    int y0, y1;
    double ds;
    S2dImage im;

    aw = bg->frameX + (int32_t)bg->frameW - lrx;
    aw = (aw > 0) ? aw : 0;
    bw = bg->frameX - ulx;
    bw = (bw < 0) ? bw : 0;
    cw = (int32_t)bg->frameW + bw - aw;
    ah = bg->frameY + (int32_t)bg->frameH - lry;
    ah = (ah > 0) ? ah : 0;
    bh = bg->frameY - uly;
    bh = (bh < 0) ? bh : 0;
    ch = (int32_t)bg->frameH + bh - ah;
    if (cw <= 0 || ch <= 0) {
        return;
    }
    /* The texture rectangle, 10.2: from the frame's upper left past the clipped part, lower right included */
    sx = bg->frameX - bw;
    sy = bg->frameY - bh;
    ex = sx + cw - 1;
    ey = sy + ch - 1;
    if (s2d_copy_mode()) {
        x0 = (float)(sx >> 2);
        x1 = (float)((ex >> 2) + 1);
        y0 = sy >> 2;
        y1 = (ey >> 2) + 1;
        ds = 1.0;
    } else {
        x0 = sx * 0.25f;
        x1 = ex * 0.25f;
        y0 = sy >> 2;
        y1 = ey >> 2;
        ds = 4.0;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    /* Image position: imageX/Y (u10.5) in whole texels, plus what was clipped; a flipped image starts at the
     * frame's right edge */
    imgX = (int32_t)((bg->imageX >> 3) & ~3u) - bw + (flip ? aw : 0);
    imgY = ((int32_t)((bg->imageY >> 3) & ~3u) - bh) >> 2;
    if (!s2d_bg_bind(bg, &im)) {
        sStats.skipped++;
        return;
    }
    sStats.bgs++;
    if (flip) {
        s2d_bg_draw(&im, x0, x1, y0, y1, imgX * 0.25 + (x1 - x0) * ds, imgY, -ds, 1.0);
    } else {
        s2d_bg_draw(&im, x0, x1, y0, y1, imgX * 0.25, imgY, ds, 1.0);
    }
}

/* G_BG_1CYC (uObjScaleBg): scaled by scaleW/scaleH texels per pixel, in the cycle type the game set. The frame is
 * cut to the scaled image's size (minus the last quarter pixel, as the microcode computes it) and to the
 * scissor; rows are whole pixels from the clipped top, as the microcode's strips are. */
static void s2d_bg_1cyc(const S2dBg* bg) {
    int32_t ulx = gGfxRdp.scissorUlx, uly = gGfxRdp.scissorUly, lrx = gGfxRdp.scissorLrx, lry = gGfxRdp.scissorLry;
    bool flip = (bg->flip & BG_FLAG_FLIPS) != 0;
    int32_t aw, bw, cw, ah, bh, ch, visW, visH, frameX;
    int64_t gw, gh;
    double ds, dt, s0;
    float x0, x1;
    int y0, y1;
    S2dImage im;

    if (bg->scaleW == 0 || bg->scaleH == 0) {
        LOG_ONCE(LOG_BG_SCALE, "gfx_s2dex: G_BG_1CYC with scale 0 not drawn (logged once)");
        sStats.skipped++;
        return;
    }
    aw = (int32_t)bg->frameW - (int32_t)(((int32_t)((bg->imageW << 10) / bg->scaleW) - 1) & ~3);
    aw = (aw > 0) ? aw : 0;
    ah = (int32_t)bg->frameH - (int32_t)(((int32_t)((bg->imageH << 10) / bg->scaleH) - 1) & ~3);
    ah = (ah > 0) ? ah : 0;
    /* A flipped image keeps its right edge in place */
    frameX = bg->frameX + (flip ? aw : 0);
    bw = ulx - frameX;
    bw = (bw > 0) ? bw : 0;
    cw = frameX + (int32_t)bg->frameW - lrx - aw;
    cw = (cw > 0) ? cw : 0;
    visW = (int32_t)bg->frameW - aw - bw - cw;
    bh = uly - bg->frameY;
    bh = (bh > 0) ? bh : 0;
    ch = bg->frameY + (int32_t)bg->frameH - lry - ah;
    ch = (ch > 0) ? ch : 0;
    visH = (int32_t)bg->frameH - ah - bh - ch;
    if (visW <= 0 || visH <= 0) {
        return;
    }
    x0 = (frameX + bw) * 0.25f;
    x1 = (frameX + bw + visW) * 0.25f;
    y0 = (bg->frameY + bh) >> 2;
    y1 = y0 + (visH >> 2);
    if (y1 <= y0) {
        return;
    }
    /* Image position (10.5) at the drawn part's upper left: imageX/Y plus the clipped part, scaled (a flipped
     * image starts at its right edge, so the part clipped on the right is skipped) */
    gw = (((int64_t)bg->scaleW * (flip ? cw : bw)) >> 7) + bg->imageX;
    gh = (((int64_t)bg->scaleH * bh) >> 7) + bg->imageY;
    if (!s2d_bg_bind(bg, &im)) {
        sStats.skipped++;
        return;
    }
    sStats.bgs++;
    ds = bg->scaleW * (1.0 / 1024.0);
    dt = bg->scaleH * (1.0 / 1024.0);
    s0 = gw * (1.0 / 32.0);
    if (flip) {
        s2d_bg_draw(&im, x0, x1, y0, y1, s0 + (x1 - x0) * ds, gh * (1.0 / 32.0), -ds, dt);
    } else {
        s2d_bg_draw(&im, x0, x1, y0, y1, s0, gh * (1.0 / 32.0), ds, dt);
    }
}

/* ============================================================================================== */
/* Objects                                                                                        */
/* ============================================================================================== */

static bool s2d_read_sprite(uint32_t addr, S2dSprite* sp) {
    const uint8_t* p = s2d_ptr(addr, 24);
    uint32_t scaleW, scaleH;

    if (p == NULL) {
        LOG_ONCE(LOG_BAD_OBJ, "gfx_s2dex: uObjSprite at bad address %08X (logged once)", (unsigned int)addr);
        return false;
    }
    /* The microcode divides by the scales: 0 behaves as the smallest step */
    scaleW = rd16(p + 2);
    scaleH = rd16(p + 10);
    sp->objX = (int16_t)rd16(p + 0) * 0.25f;
    sp->scaleW = ((scaleW != 0) ? scaleW : 1) * (1.0f / 1024.0f);
    sp->imageW = rd16(p + 4) * (1.0f / 32.0f);
    sp->objY = (int16_t)rd16(p + 8) * 0.25f;
    sp->scaleH = ((scaleH != 0) ? scaleH : 1) * (1.0f / 1024.0f);
    sp->imageH = rd16(p + 12) * (1.0f / 32.0f);
    sp->stride = rd16(p + 16);
    sp->adrs = rd16(p + 18);
    sp->fmt = p[20] & 7;
    sp->siz = p[21] & 3;
    sp->pal = p[22] & 0xF;
    sp->flags = p[23];
    return true;
}

/* The render tile of a sprite: texels at TMEM word imageAdrs, imageStride words per row, clamped at the image's
 * size unless G_OBJRM_NOTXCLAMP */
static void s2d_sprite_tile(const S2dSprite* sp) {
    uint32_t w = (uint32_t)sp->imageW, h = (uint32_t)sp->imageH;
    uint32_t cm = (sS2d.renderMode & OBJRM_NOTXCLAMP) ? G_TX_WRAP : G_TX_CLAMP;

    w = (w != 0) ? w : 1;
    h = (h != 0) ? h : 1;
    s2d_settile(G_TX_RENDERTILE, sp->fmt, sp->siz, sp->stride, sp->adrs, sp->pal, cm);
    s2d_tile_cmd(G_SETTILESIZE, G_TX_RENDERTILE, 0, 0, (w - 1) << 2, (h - 1) << 2);
}

/* A sprite as a texture rectangle: screen [x0, x1) x [y0, y1) (pixels), ds/dt texels per pixel (negative when the
 * screen mirrors it), texel (0, 0) at the upper left unless flipped. Clipped to the RDP's unsigned 10.2
 * coordinates; in copy mode the lower right edge is included and dsdx counts 4 texels per pixel. */
static void s2d_sprite_rect(const S2dSprite* sp, float x0, float y0, float x1, float y1, float ds, float dt) {
    bool copy = s2d_copy_mode();
    float s, t;
    int32_t ulx, uly, lrx, lry;

    if (sp->flags & OBJ_FLAG_FLIPS) {
        ds = -ds;
    }
    if (sp->flags & OBJ_FLAG_FLIPT) {
        dt = -dt;
    }
    /* Texel coordinate at the left / top edge: the image's last texel when it runs backwards */
    s = (ds < 0.0f) ? sp->imageW - 1.0f : 0.0f;
    t = (dt < 0.0f) ? sp->imageH - 1.0f : 0.0f;
    if (x0 < 0.0f) {
        s -= x0 * ds;
        x0 = 0.0f;
    }
    if (y0 < 0.0f) {
        t -= y0 * dt;
        y0 = 0.0f;
    }
    x1 = (x1 < 1023.75f) ? x1 : 1023.75f;
    y1 = (y1 < 1023.75f) ? y1 : 1023.75f;
    ulx = (int32_t)(x0 * 4.0f);
    uly = (int32_t)(y0 * 4.0f);
    lrx = (int32_t)(x1 * 4.0f);
    lry = (int32_t)(y1 * 4.0f);
    if (copy) {
        lrx -= 4;
        lry -= 4;
        ds *= 4.0f;
    }
    if (lrx < ulx || lry < uly || (!copy && (lrx == ulx || lry == uly))) {
        return;
    }
    s2d_sprite_tile(sp);
    gfx_rdp_texrect(((uint32_t)G_TEXRECT << 24) | ((uint32_t)lrx << 12) | (uint32_t)lry,
                    ((uint32_t)G_TX_RENDERTILE << 24) | ((uint32_t)ulx << 12) | (uint32_t)uly,
                    (((uint32_t)clamp_s16(s * 32.0f) & 0xFFFF) << 16) | ((uint32_t)clamp_s16(t * 32.0f) & 0xFFFF),
                    (((uint32_t)clamp_s16(ds * 1024.0f) & 0xFFFF) << 16) | ((uint32_t)clamp_s16(dt * 1024.0f) & 0xFFFF),
                    false);
    sStats.objs++;
}

/* G_OBJ_RECTANGLE: at (objX, objY), imageW / scaleW pixels wide; the 2D matrix does not apply */
static void s2d_obj_rectangle(const S2dSprite* sp) {
    s2d_sprite_rect(sp, sp->objX, sp->objY, sp->objX + sp->imageW / sp->scaleW, sp->objY + sp->imageH / sp->scaleH,
                    sp->scaleW, sp->scaleH);
}

/* G_OBJ_RECTANGLE_R: the 2D matrix's position and base scale apply (object coordinates / BaseScale + X, Y) */
static void s2d_obj_rectangle_r(const S2dSprite* sp) {
    const S2dMtx* m = &sS2d.mtx;
    float bx = (m->baseScaleX > 0.0f) ? m->baseScaleX : 1.0f / 1024.0f;
    float by = (m->baseScaleY > 0.0f) ? m->baseScaleY : 1.0f / 1024.0f;
    float x0 = sp->objX / bx + m->x, y0 = sp->objY / by + m->y;

    s2d_sprite_rect(sp, x0, y0, x0 + sp->imageW / sp->scaleW / bx, y0 + sp->imageH / sp->scaleH / by,
                    sp->scaleW * bx, sp->scaleH * by);
}

/* G_OBJ_SPRITE: the object rectangle transformed by the 2D matrix, screen = (X, Y) + [A B; C D] * object. An axis
 * aligned matrix gives a texture rectangle; a rotation or shear two triangles. */
static void s2d_obj_sprite(const S2dSprite* sp) {
    static const float sIdentity[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    const S2dMtx* m = &sS2d.mtx;
    float ow = sp->imageW / sp->scaleW, oh = sp->imageH / sp->scaleH;
    float vsx = gGfxRsp.viewportScale[0] * 0.25f, vsy = gGfxRsp.viewportScale[1] * 0.25f;
    float vtx = gGfxRsp.viewportTrans[0] * 0.25f, vty = gGfxRsp.viewportTrans[1] * 0.25f;
    GfxVtx v[4];
    int i;

    if (m->b == 0.0f && m->c == 0.0f) {
        float xa = m->x + m->a * sp->objX, xb = m->x + m->a * (sp->objX + ow);
        float ya = m->y + m->d * sp->objY, yb = m->y + m->d * (sp->objY + oh);
        float ds, dt;

        if (m->a == 0.0f || m->d == 0.0f) {
            return;
        }
        ds = sp->scaleW / m->a;
        dt = sp->scaleH / m->d;
        /* A negative scale mirrors: the rectangle starts at the far corner and its texels run backwards */
        s2d_sprite_rect(sp, (xa < xb) ? xa : xb, (ya < yb) ? ya : yb, (xa < xb) ? xb : xa, (ya < yb) ? yb : ya, ds,
                        dt);
        return;
    }

    /* Corners UL, UR, LL, LR to NDC through the viewport (gx_viewport_n64's default when there is none) */
    if (vsx == 0.0f || vsy == 0.0f) {
        vsx = vtx = GFX_N64_WIDTH / 2;
        vsy = vty = GFX_N64_HEIGHT / 2;
    }
    memset(v, 0, sizeof(v));
    for (i = 0; i < 4; i++) {
        float ox = sp->objX + ((i & 1) ? ow : 0.0f), oy = sp->objY + ((i & 2) ? oh : 0.0f);
        float px = m->x + m->a * ox + m->b * oy, py = m->y + m->c * ox + m->d * oy;
        bool right = ((i & 1) != 0) != ((sp->flags & OBJ_FLAG_FLIPS) != 0);
        bool bottom = ((i & 2) != 0) != ((sp->flags & OBJ_FLAG_FLIPT) != 0);

        v[i].x = (px - vtx) / vsx;
        v[i].y = (vty - py) / vsy;
        v[i].z = -1.0f;
        v[i].w = 1.0f;
        v[i].s = right ? sp->imageW : 0.0f;
        v[i].t = bottom ? sp->imageH : 0.0f;
        v[i].r = v[i].g = v[i].b = v[i].a = 255;
    }
    s2d_sprite_tile(sp);
    gfx_gx_set_projection(sIdentity);
    gfx_gx_triangle(&v[0], &v[1], &v[2]);
    gfx_gx_triangle(&v[2], &v[1], &v[3]);
    sStats.objs++;
}

/* G_OBJ_LOADTXTR (uObjTxtr): load TMEM through G_TX_LOADTILE as 16-bit texels, unless the status word already says
 * the texture is there ((status[sid] & mask) == flag); then the status takes the flag */
static void s2d_obj_loadtxtr(uint32_t addr) {
    const uint8_t* p = s2d_ptr(addr, UOBJTXTR_SIZE);
    uint32_t type, image, a, b, c, sid, flag, mask;

    if (p == NULL) {
        LOG_ONCE(LOG_TXTR, "gfx_s2dex: uObjTxtr at bad address %08X (logged once)", (unsigned int)addr);
        return;
    }
    type = rd32(p);
    image = rd32(p + 4);
    a = rd16(p + 8);
    b = rd16(p + 10);
    c = rd16(p + 12);
    sid = rd16(p + 14);
    flag = rd32(p + 16);
    mask = rd32(p + 20);
    if (sid > 12 || (sid & 3)) {
        LOG_ONCE(LOG_SID, "gfx_s2dex: status id %u out of range (logged once)", (unsigned int)sid);
        return;
    }
    if ((sS2d.status[sid >> 2] & mask) == flag) {
        return;
    }
    switch (type) {
        case OBJLT_TXTRBLOCK: /* tmem, tsize (64-bit words - 1), tline (dxt) */
            s2d_settimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, image);
            s2d_settile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, a, 0, 0);
            s2d_tile_cmd(G_LOADBLOCK, G_TX_LOADTILE, 0, 0, (b << 2) | 3, c);
            break;
        case OBJLT_TXTRTILE: /* tmem, twidth (16-bit texels per row - 1), theight (rows, 10.2, - 1) */
            s2d_settimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, b + 1, image);
            s2d_settile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_16b, (b + 1) >> 2, a, 0, 0);
            s2d_tile_cmd(G_LOADTILE, G_TX_LOADTILE, 0, 0, b << 2, c);
            break;
        case OBJLT_TLUT: /* phead (256 + first entry), pnum (entries - 1) */
            s2d_settimg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, image);
            s2d_settile(G_TX_LOADTILE, G_IM_FMT_RGBA, G_IM_SIZ_4b, 0, a, 0, 0);
            s2d_tile_cmd(G_LOADTLUT, G_TX_LOADTILE, 0, 0, b << 2, 0);
            gfx_s2dex_note_tlut(((uint32_t)G_LOADTLUT << 24), ((uint32_t)G_TX_LOADTILE << 24) | ((b << 2) << 12));
            break;
        default:
            LOG_ONCE(LOG_TXTR, "gfx_s2dex: uObjTxtr type %08X ignored (logged once)", (unsigned int)type);
            return;
    }
    sS2d.status[sid >> 2] = (sS2d.status[sid >> 2] & ~mask) | (flag & mask);
}

/* uObjSubMtx, also the end of uObjMtx: X, Y (s10.2), BaseScaleX, BaseScaleY (u5.10) */
static void s2d_read_submtx(const uint8_t* p) {
    S2dMtx* m = &sS2d.mtx;

    m->x = (int16_t)rd16(p) * 0.25f;
    m->y = (int16_t)rd16(p + 2) * 0.25f;
    m->baseScaleX = rd16(p + 4) * (1.0f / 1024.0f);
    m->baseScaleY = rd16(p + 6) * (1.0f / 1024.0f);
}

/* G_OBJ_MOVEMEM: gSPObjMatrix (uObjMtx: A, B, C, D in s15.16, then the sub-matrix) or gSPObjSubMatrix */
static void s2d_obj_movemem(uint32_t w0, uint32_t w1) {
    S2dMtx* m = &sS2d.mtx;
    uint32_t which = w0 & 0xFFFF;
    const uint8_t* p = s2d_ptr(w1, (which == S2D_MV_MATRIX) ? 24 : 8);

    if (p != NULL && which == S2D_MV_MATRIX) {
        m->a = (int32_t)rd32(p) * (1.0f / 65536.0f);
        m->b = (int32_t)rd32(p + 4) * (1.0f / 65536.0f);
        m->c = (int32_t)rd32(p + 8) * (1.0f / 65536.0f);
        m->d = (int32_t)rd32(p + 12) * (1.0f / 65536.0f);
        s2d_read_submtx(p + 16);
    } else if (p != NULL && which == S2D_MV_SUBMATRIX) {
        s2d_read_submtx(p);
    } else {
        LOG_ONCE(LOG_MOVEMEM, "gfx_s2dex: G_OBJ_MOVEMEM %08X %08X ignored (logged once)", (unsigned int)w0,
                 (unsigned int)w1);
    }
}

/* ============================================================================================== */
/* Entry points                                                                                   */
/* ============================================================================================== */

void gfx_s2dex_load(void) {
    memset(&sS2d, 0, sizeof(sS2d));
    sS2d.mtx.a = sS2d.mtx.d = 1.0f;
    sS2d.mtx.baseScaleX = sS2d.mtx.baseScaleY = 1.0f;
}

void gfx_s2dex_reset(void) {
    gfx_s2dex_load();
    /* TMEM's TLUTs as the TMEM model sees them: loaded in this task (gfx_tex_reset) */
    memset(sTluts, 0, sizeof(sTluts));
}

bool gfx_s2dex_command(uint32_t w0, uint32_t w1) {
    S2dBg bg;
    S2dSprite sp;

    switch (w0 >> 24) {
        case S2D_BG_COPY:
            if (s2d_read_bg(w1, &bg)) {
                s2d_bg_copy(&bg);
            }
            return true;
        case S2D_BG_1CYC:
            if (s2d_read_bg(w1, &bg)) {
                s2d_bg_1cyc(&bg);
            }
            return true;
        case S2D_OBJ_RENDERMODE:
            sS2d.renderMode = w1;
            return true;
        case S2D_OBJ_MOVEMEM:
            s2d_obj_movemem(w0, w1);
            return true;
        case S2D_OBJ_LOADTXTR:
            s2d_obj_loadtxtr(w1);
            return true;
        case S2D_OBJ_RECTANGLE:
        case S2D_OBJ_RECTANGLE_R:
        case S2D_OBJ_SPRITE:
            if (s2d_read_sprite(w1, &sp)) {
                if ((w0 >> 24) == S2D_OBJ_RECTANGLE) {
                    s2d_obj_rectangle(&sp);
                } else if ((w0 >> 24) == S2D_OBJ_RECTANGLE_R) {
                    s2d_obj_rectangle_r(&sp);
                } else {
                    s2d_obj_sprite(&sp);
                }
            }
            return true;
        case S2D_OBJ_LDTX_RECT:
        case S2D_OBJ_LDTX_RECT_R:
        case S2D_OBJ_LDTX_SPRITE:
            /* uObjTxSprite: the uObjTxtr, then the uObjSprite */
            s2d_obj_loadtxtr(w1);
            if (s2d_read_sprite(w1 + UOBJTXTR_SIZE, &sp)) {
                if ((w0 >> 24) == S2D_OBJ_LDTX_RECT) {
                    s2d_obj_rectangle(&sp);
                } else if ((w0 >> 24) == S2D_OBJ_LDTX_RECT_R) {
                    s2d_obj_rectangle_r(&sp);
                } else {
                    s2d_obj_sprite(&sp);
                }
            }
            return true;
        default:
            return false;
    }
}

bool gfx_s2dex_select_dl(uint32_t half0W0, uint32_t half0W1, uint32_t w0, uint32_t w1, uint32_t* dl, bool* push) {
    uint32_t sid = (half0W0 >> 16) & 0xFF;
    uint32_t flag = half0W1, mask = w1;
    uint32_t* status;

    if (sid > 12 || (sid & 3)) {
        LOG_ONCE(LOG_SID, "gfx_s2dex: status id %u out of range (logged once)", (unsigned int)sid);
        return false;
    }
    status = &sS2d.status[sid >> 2];
    if ((*status & mask) == flag) {
        return false;
    }
    *status = (*status & ~mask) | (flag & mask);
    *dl = ((w0 & 0xFFFF) << 16) | (half0W0 & 0xFFFF);
    *push = ((w0 >> 16) & 0xFF) == G_DL_PUSH;
    return true;
}

void gfx_s2dex_set_status(uint32_t ofs, uint32_t value) {
    if (ofs > 12 || (ofs & 3)) {
        LOG_ONCE(LOG_SID, "gfx_s2dex: status id %u out of range (logged once)", (unsigned int)ofs);
        return;
    }
    sS2d.status[ofs >> 2] = value;
}

void gfx_s2dex_get_stats(GfxS2dexStats* out) {
    *out = sStats;
}

/**
 * Dolphin test DOL for port/gc/gfx/gfx_tev.c: the combine and render mode tables of tev_cases.h drawn through
 * the real GX. Every random sample is an 8x8 cell: a background quad in the "framebuffer" color, then a quad
 * drawn with the state gfx_tev_apply() programmed (uniform 4x4 textures for TEXEL0/TEXEL1, vertex color as
 * shade). The EFB is then copied into a RAM texture (GX_CopyTex) and every cell is compared with the RDP
 * model (rdp_model.c). Results go to the text console and the USB Gecko.
 *
 *   make -C port/gc/tests/gfx_tev_host dol
 *
 * The readback needs EFB copies that reach RAM. In Dolphin that means the software renderer (whose GX is
 * also the most exact) with "Store EFB Copies to Texture Only" off; port/gc/tools/run_dolphin.ps1 does not set
 * these, so run Dolphin with these extra arguments (per run, not saved):
 *   -C "Dolphin.Core.GFXBackend=Software Renderer" -C GFX.Hacks.EFBToTextureEnable=False
 * With Dolphin's hardware backends the copy comes back black and the test stops at its setup check.
 */
#include <gccore.h>
#include <malloc.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_internal.h"
#include "rdp_model.h"
#include "tev_cases.h"

GfxRdpState gGfxRdp;
GfxRspState gGfxRsp;

#define CELL 8
#define COLS (640 / CELL)
#define ROWS (480 / CELL)
#define SAMPLES 6
#define MAX_CELLS (COLS * ROWS)
#define GECKO_CHANNEL 1

typedef struct {
    const char* name;
    int config;
    C4 expect, mem;
    int tol;
    N64In in;
} Cell;

static GXRModeObj* sMode;
static void* sXfb;
static int sGecko;
static Cell* sCells;
static u8* sTexMem;
static int sNumCells;
static u8* sReadback; /* EFB as an RGBA8 texture */
static uint32_t sRng = 0x2545F491u;

void gc_log(const char* fmt, ...) {
    char line[320];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    line[n] = '\0';
    fputs(line, stdout);
    if (sGecko) {
        usb_sendbuffer_safe(GECKO_CHANNEL, line, n);
    }
}

static uint32_t rng(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static int rnd8(void) {
    uint32_t r = rng() % 100;
    return r < 8 ? 0 : r < 16 ? 255 : (int)(rng() & 0xFF);
}

static C4 rnd_c4(void) {
    C4 c = { rnd8(), rnd8(), rnd8(), rnd8() };
    return c;
}

static uint32_t pack(C4 c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

static void video_init(void) {
    VIDEO_Init();
    sMode = VIDEO_GetPreferredMode(NULL);
    sXfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    console_init(sXfb, 20, 20, sMode->fbWidth, sMode->xfbHeight, sMode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(sMode);
    VIDEO_SetNextFramebuffer(sXfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (sMode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

static void gx_init(void) {
    static const GXColor black = { 0, 0, 0, 255 };
    void* fifo = memalign(32, 256 * 1024);
    Mtx44 proj;
    Mtx mv;
    f32 yscale;
    u32 xfbHeight;
    int i;

    memset(fifo, 0, 256 * 1024);
    GX_Init(fifo, 256 * 1024);
    GX_SetCopyClear(black, GX_MAX_Z24);
    GX_SetViewport(0, 0, sMode->fbWidth, sMode->efbHeight, 0, 1);
    yscale = GX_GetYScaleFactor(sMode->efbHeight, sMode->xfbHeight);
    xfbHeight = GX_SetDispCopyYScale(yscale);
    GX_SetScissor(0, 0, sMode->fbWidth, sMode->efbHeight);
    GX_SetDispCopySrc(0, 0, sMode->fbWidth, sMode->efbHeight);
    GX_SetDispCopyDst(sMode->fbWidth, xfbHeight);
    GX_SetCopyFilter(sMode->aa, sMode->sample_pattern, GX_TRUE, sMode->vfilter);
    GX_SetFieldMode(sMode->field_rendering, (sMode->viHeight == 2 * sMode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_CopyDisp(sXfb, GX_TRUE);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);

    guOrtho(proj, 0, 480, 0, 640, 0, 300);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    GX_SetNumTexGens(2);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumIndStages(0);
    for (i = 0; i < GX_MAX_TEVSTAGE; i++) {
        GX_SetTevDirect(i);
    }
    GX_InvalidateTexAll();
}

static void vertex(f32 x, f32 y, C4 c) {
    GX_Position3f32(x, y, -1.0f);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0.5f, 0.5f);
    GX_TexCoord2f32(0.5f, 0.5f);
}

static void draw_quad(int x, int y, C4 c) {
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    vertex(x, y, c);
    vertex(x + CELL, y, c);
    vertex(x + CELL, y + CELL, c);
    vertex(x, y + CELL, c);
    GX_End();
}

/* The cell's "framebuffer" color: vertex color straight through */
static void background_state(void) {
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    gfx_tev_invalidate();
}

/* Uniform 4x4 RGBA8 texture: one tile, 16 AR pairs then 16 GB pairs */
static void load_texture(u8* buf, C4 c, int map) {
    GXTexObj obj;
    int i;

    for (i = 0; i < 16; i++) {
        buf[i * 2] = (u8)c.a;
        buf[i * 2 + 1] = (u8)c.r;
        buf[32 + i * 2] = (u8)c.g;
        buf[32 + i * 2 + 1] = (u8)c.b;
    }
    DCFlushRange(buf, 64);
    GX_InitTexObj(&obj, buf, 4, 4, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&obj, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&obj, map);
}

#define READBACK_SIZE (640 * 480 * 4)

/* EFB -> RGBA8 texture in RAM (4x4 tiles: 16 AR pairs, then 16 GB pairs) */
static void readback(void) {
    memset(sReadback, 0x5A, READBACK_SIZE);
    DCFlushRange(sReadback, READBACK_SIZE);
    GX_SetTexCopySrc(0, 0, 640, 480);
    GX_SetTexCopyDst(640, 480, GX_TF_RGBA8, GX_FALSE);
    GX_CopyTex(sReadback, GX_FALSE);
    GX_PixModeSync();
    GX_DrawDone();
    DCInvalidateRange(sReadback, READBACK_SIZE);
}

static C4 readback_pixel(int x, int y) {
    const u8* tile = &sReadback[((y / 4) * (640 / 4) + (x / 4)) * 64];
    int i = (y % 4) * 4 + (x % 4);
    C4 c = { tile[i * 2 + 1], tile[32 + i * 2], tile[32 + i * 2 + 1], tile[i * 2] };
    return c;
}

static void draw_config(const char* name, int config, uint32_t hi, uint32_t lo, uint32_t cycle, uint32_t modeL,
                        GfxPrimKind kind, uint32_t geom, int tol) {
    bool shadeAvail = (kind == GFX_PRIM_TRIANGLE) && (geom & G_SHADE);
    int s;

    for (s = 0; s < SAMPLES && sNumCells < MAX_CELLS; s++) {
        Cell* cell = &sCells[sNumCells];
        int x = (sNumCells % COLS) * CELL, y = (sNumCells / COLS) * CELL;
        N64In in;
        N64Out n;
        GfxTevInfo info;
        C4 shade;
        int tries;

        /* Samples the RDP wraps or that sit on an alpha threshold are not comparable (see test_tev.c) */
        for (tries = 0; tries < 100; tries++) {
            in.tex0 = rnd_c4();
            in.tex1 = rnd_c4();
            in.shade = rnd_c4();
            in.prim = rnd_c4();
            in.env = rnd_c4();
            in.fog = rnd_c4();
            in.blend = rnd_c4();
            in.mem = rnd_c4();
            in.mem.a = 0xE0;
            in.primLod = rnd8();
            rdp_pixel(hi, lo, cycle | G_TP_PERSP | G_TF_BILERP, modeL, shadeAvail, &in, &n);
            if (!n.overflow && !(n.acValue >= 0 && abs(n.acValue - in.blend.a) <= 3) &&
                !(n.cutout && abs(n.rawAlpha - 32) <= 3)) {
                break;
            }
        }
        if (tries == 100) {
            continue;
        }

        background_state();
        draw_quad(x, y, in.mem);

        gGfxRdp.combineHi = hi;
        gGfxRdp.combineLo = lo;
        gGfxRdp.otherModeH = cycle | G_TP_PERSP | G_TF_BILERP;
        gGfxRdp.otherModeL = modeL;
        gGfxRdp.primColor = pack(in.prim);
        gGfxRdp.envColor = pack(in.env);
        gGfxRdp.fogColor = pack(in.fog);
        gGfxRdp.blendColor = pack(in.blend);
        gGfxRdp.primLodFrac = (uint8_t)in.primLod;
        gGfxRdp.dirty = GFX_DIRTY_COMBINE | GFX_DIRTY_OTHERMODE | GFX_DIRTY_COLORS;
        gGfxRsp.geometryMode = geom;
        gfx_tev_apply(kind, &info);
        gGfxRdp.dirty = 0;
        load_texture(&sTexMem[sNumCells * 128], in.tex0, GX_TEXMAP0);
        load_texture(&sTexMem[sNumCells * 128 + 64], in.tex1, GX_TEXMAP1);
        shade = in.shade;
        if (!shadeAvail) {
            C4 garbage = { 77, 99, 11, 201 };
            shade = garbage;
        }
        draw_quad(x, y, shade);

        cell->name = name;
        cell->config = config;
        cell->expect = n.color;
        cell->mem = in.mem;
        cell->tol = tol;
        cell->in = in;
        sNumCells++;
    }
}

int main(void) {
    size_t i, v;
    int config = 0, c, bad = 0, badConfigs = 0, lastBadConfig = -1, worst = 0;

    video_init();
    sGecko = usb_isgeckoalive(GECKO_CHANNEL);
    gc_log("dol_tev: GX TEV programs of gfx_tev.c vs the RDP model, %d samples per mode", SAMPLES);
    sCells = calloc(MAX_CELLS, sizeof(Cell));
    sTexMem = memalign(32, MAX_CELLS * 128);
    sReadback = memalign(32, READBACK_SIZE);
    gx_init();
    gfx_tev_init();

    /* The pipeline itself: one red cell drawn with a plain TEV setup, read back inside and outside */
    {
        C4 red = { 255, 0, 0, 255 };
        C4 in, out;

        background_state();
        draw_quad(0, 0, red);
        readback();
        in = readback_pixel(CELL / 2, CELL / 2);
        out = readback_pixel(CELL * 3, CELL * 3);
        gc_log("dol_tev: setup check: cell %d %d %d (red expected), outside %d %d %d (black expected)", in.r, in.g,
               in.b, out.r, out.g, out.b);
        {
            int changed = 0, j;

            for (j = 0; j < READBACK_SIZE; j++) {
                changed += sReadback[j] != 0x5A;
            }
            gc_log("dol_tev: readback wrote %d of %d bytes; first bytes %02X %02X %02X %02X / %02X %02X %02X %02X",
                   changed, READBACK_SIZE, sReadback[0], sReadback[1], sReadback[2], sReadback[3], sReadback[32],
                   sReadback[33], sReadback[34], sReadback[35]);
        }
        if (in.r != 255 || in.g != 0 || out.r != 0) {
            gc_log("dol_tev: FAIL (drawing or the EFB copy to RAM does not work; Dolphin needs "
                   "GFX.Hacks.EFBToTextureEnable=False)");
            GX_CopyDisp(sXfb, GX_FALSE);
            GX_DrawDone();
            for (;;) {
                VIDEO_WaitVSync();
            }
        }
    }

    for (i = 0; i < sizeof(sCombCases) / sizeof(sCombCases[0]); i++) {
        const CombCase* cc = &sCombCases[i];

        for (v = 0; v < sizeof(sModeVariants) / sizeof(sModeVariants[0]); v++) {
            const ModeVariant* mv = &sModeVariants[v];

            draw_config(cc->name, config++, cc->hi, cc->lo, cc->cycle,
                        cc->cycle == G_CYC_2CYCLE ? mv->modeL2 : mv->modeL1, cc->kind, cc->geom,
                        mv->checkAlpha ? 10 : mv->tol);
        }
    }
    for (i = 0; i < sizeof(sBlendCases) / sizeof(sBlendCases[0]); i++) {
        const BlendCase* bc = &sBlendCases[i];

        draw_config(bc->name, config++, bc->hi, bc->lo, bc->cycle, bc->modeL, bc->kind, bc->geom, 16);
    }
    readback();

    for (c = 0; c < sNumCells; c++) {
        const Cell* cell = &sCells[c];
        int x = (c % COLS) * CELL + CELL / 2, y = (c / COLS) * CELL + CELL / 2;
        C4 px = readback_pixel(x, y);
        int d;

        d = abs(px.r - cell->expect.r);
        d = abs(px.g - cell->expect.g) > d ? abs(px.g - cell->expect.g) : d;
        d = abs(px.b - cell->expect.b) > d ? abs(px.b - cell->expect.b) : d;
        worst = d > worst ? d : worst;
        if (d > cell->tol) {
            bad++;
            if (cell->config != lastBadConfig) {
                const N64In* in = &cell->in;

                badConfigs++;
                lastBadConfig = cell->config;
                gc_log("MISMATCH %s (config %d): GX %d %d %d, RDP %d %d %d, framebuffer %d %d %d", cell->name,
                       cell->config, px.r, px.g, px.b, cell->expect.r, cell->expect.g, cell->expect.b, cell->mem.r,
                       cell->mem.g, cell->mem.b);
                gc_log("  t0 %d %d %d %d t1 %d %d %d %d shade %d %d %d %d prim %d %d %d %d env %d %d %d %d fog %d %d %d %d "
                       "blend %d %d %d %d lod %d",
                       in->tex0.r, in->tex0.g, in->tex0.b, in->tex0.a, in->tex1.r, in->tex1.g, in->tex1.b, in->tex1.a,
                       in->shade.r, in->shade.g, in->shade.b, in->shade.a, in->prim.r, in->prim.g, in->prim.b,
                       in->prim.a, in->env.r, in->env.g, in->env.b, in->env.a, in->fog.r, in->fog.g, in->fog.b,
                       in->fog.a, in->blend.r, in->blend.g, in->blend.b, in->blend.a, in->primLod);
            }
        }
    }

    GX_CopyDisp(sXfb, GX_TRUE);
    GX_DrawDone();
    gc_log("dol_tev: %d modes, %d cells, %d mismatching cells in %d modes, largest difference %d", config, sNumCells,
           bad, badConfigs, worst);
    gc_log(bad == 0 ? "dol_tev: PASS" : "dol_tev: FAIL");

    for (;;) {
        VIDEO_WaitVSync();
    }
    return 0;
}

/**
 * Mocks for the gfx_rsp / gfx_s2dex host test: fake N64 RAM, the bridge (log, time), the GX backend, the RDP
 * module (recording commands and keeping the state S2DEX2 reads), the texture cache and gfx_fb.c entry points,
 * gGfxRdp, and the dummy microcode symbols. The GX mocks count vertices, matrices or rectangles that are not
 * finite.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x01800000u

uintptr_t gGfxHostRamBias;
static uint8_t* sRam;
static uint32_t sRamTop;

unsigned char gspS2DEX2_fifoTextStart[16];
unsigned char gspF3DZEX2_NoN_PosLight_fifoTextStart[16];

GfxRdpState gGfxRdp;

TriRec gTris[MAX_TRIS];
int gTriCount;
float gProj[4][4];
int gProjCount;
CmdRec gRdp[MAX_RDP];
int gRdpCount;
TexrectRec gTexrects[MAX_TEXRECTS];
int gTexrectCount;
BindRec gBinds[MAX_BINDS];
int gBindCount;
bool gBindFail;
ImageRectRec gImageRects[MAX_IMAGE_RECTS];
int gImageRectCount;
bool gFbReady;
bool gFbBindImage;
int gFbBindCount;
const void* gFbBindAddr;
bool gFbBindLinear;
GfxBindImageFn gFbBindFromRam;
bool gFbBound;
int gFbDoneCount, gFbDoneRects;
int gFbSyncCount;
const void* gFbSyncAddr;
uint32_t gFbSyncBytes;
int gChecks, gFailures;
int gLogCount;
char gLastLog[512];
char gPrevLog[512];
int gNonFinite;
GfxTexStats gTexStats;

static unsigned long long sTicks;

void ram_init(void) {
    sRam = aligned_alloc(4096, RAM_SIZE);
    if (sRam == NULL) {
        fprintf(stderr, "cannot allocate fake RAM\n");
        exit(2);
    }
    gGfxHostRamBias = (uintptr_t)sRam - RAM_BASE;
    ram_reset();
}

void ram_reset(void) {
    memset(sRam, 0, 1 << 20); /* tests only use the first MiB */
    sRamTop = 0x1000;
}

uint32_t ram_alloc(uint32_t size) {
    uint32_t addr = RAM_BASE + sRamTop;

    sRamTop = (sRamTop + size + 15) & ~15u;
    if (sRamTop > (1u << 20)) {
        fprintf(stderr, "fake RAM exhausted\n");
        exit(2);
    }
    memset(sRam + (addr - RAM_BASE), 0, size);
    return addr;
}

uint8_t* ram_ptr(uint32_t addr) {
    return sRam + ((addr & 0x1FFFFFFF) - (RAM_BASE & 0x1FFFFFFF));
}

void wr16(uint32_t addr, uint16_t v) {
    uint8_t* p = ram_ptr(addr);

    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void wr32(uint32_t addr, uint32_t v) {
    uint8_t* p = ram_ptr(addr);

    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void mock_reset(void) {
    gTriCount = 0;
    gProjCount = 0;
    gRdpCount = 0;
    gTexrectCount = 0;
    gBindCount = 0;
    gImageRectCount = 0;
    gFbBindCount = 0;
    gFbBindAddr = NULL;
    gFbBindLinear = false;
    gFbBindFromRam = NULL;
    gFbBound = false;
    gFbDoneCount = 0;
    gFbDoneRects = -1;
    gFbSyncCount = 0;
    gFbSyncAddr = NULL;
    gFbSyncBytes = 0;
    memset(&gGfxRdp, 0, sizeof(gGfxRdp));
    gGfxRdp.scissorLrx = GFX_N64_WIDTH << 2;
    gGfxRdp.scissorLry = GFX_N64_HEIGHT << 2;
}

/* Bridge */

void gc_log(const char* fmt, ...) {
    va_list ap;

    memcpy(gPrevLog, gLastLog, sizeof(gPrevLog));
    va_start(ap, fmt);
    vsnprintf(gLastLog, sizeof(gLastLog), fmt, ap);
    va_end(ap);
    gLogCount++;
    if (getenv("GFX_TEST_VERBOSE") != NULL) {
        printf("  [log] %s\n", gLastLog);
    }
}

void gc_halt(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

unsigned long long gc_time_ticks(void) {
    sTicks += 4050; /* 100 us per call */
    return sTicks;
}

/* GX backend and RDP module */

void gfx_gx_set_projection(const float m[4][4]) {
    int i;

    for (i = 0; i < 16; i++) {
        if (!isfinite(m[i >> 2][i & 3])) {
            gNonFinite++;
        }
    }
    memcpy(gProj, m, sizeof(gProj));
    gProjCount++;
}

static void check_finite(const GfxVtx* v) {
    if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) || !isfinite(v->w) || !isfinite(v->s) ||
        !isfinite(v->t)) {
        gNonFinite++;
    }
}

void gfx_gx_triangle(const GfxVtx* v0, const GfxVtx* v1, const GfxVtx* v2) {
    check_finite(v0);
    check_finite(v1);
    check_finite(v2);
    if (gTriCount < MAX_TRIS) {
        gTris[gTriCount].v[0] = *v0;
        gTris[gTriCount].v[1] = *v1;
        gTris[gTriCount].v[2] = *v2;
    }
    gTriCount++;
}

/* Segmented or KSEG address as gfx_rdp.c keeps G_SETTIMG's */
static uint32_t mock_resolve(uint32_t addr) {
    if (addr == 0 || (addr & 0x80000000)) {
        return (addr == 0) ? 0 : (0x80000000 | (addr & 0x1FFFFFFF));
    }
    return 0x80000000 | (gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF));
}

/* F3DEX2 G_SETOTHERMODE_H/L: replace len bits at 32 - sft - len */
static uint32_t mock_othermode(uint32_t word, uint32_t w0, uint32_t w1) {
    int len = (int)(w0 & 0xFF) + 1;
    int sft = 32 - (int)((w0 >> 8) & 0xFF) - len;
    uint32_t mask;

    if (sft < 0) {
        return word;
    }
    mask = (len >= 32) ? 0xFFFFFFFF : (((1u << len) - 1) << sft);
    return (word & ~mask) | w1;
}

void gfx_rdp_command(uint32_t w0, uint32_t w1) {
    GfxTile* t = &gGfxRdp.tiles[(w1 >> 24) & 7];

    if (gRdpCount < MAX_RDP) {
        gRdp[gRdpCount].w0 = w0;
        gRdp[gRdpCount].w1 = w1;
    }
    gRdpCount++;
    switch (w0 >> 24) {
        case G_SETTIMG:
            gGfxRdp.texImageAddr = mock_resolve(w1);
            gGfxRdp.texImageFmt = (w0 >> 21) & 7;
            gGfxRdp.texImageSiz = (w0 >> 19) & 3;
            gGfxRdp.texImageWidth = (w0 & 0x3FF) + 1;
            break;
        case G_SETTILE:
            t->fmt = (w0 >> 21) & 7;
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 0x1FF;
            t->tmem = w0 & 0x1FF;
            t->palette = (w1 >> 20) & 0xF;
            t->cmt = (w1 >> 18) & 3;
            t->cms = (w1 >> 8) & 3;
            break;
        case G_SETTILESIZE:
            t->uls = (w0 >> 12) & 0xFFF;
            t->ult = w0 & 0xFFF;
            t->lrs = (w1 >> 12) & 0xFFF;
            t->lrt = w1 & 0xFFF;
            break;
        case G_SETCIMG:
            gGfxRdp.colorImageAddr = w1;
            gGfxRdp.colorImageWidth = (w0 & 0x3FF) + 1;
            break;
        case G_SETZIMG:
            gGfxRdp.zImageAddr = w1;
            break;
        case G_RDPSETOTHERMODE:
            gGfxRdp.otherModeH = w0 & 0x00FFFFFF;
            gGfxRdp.otherModeL = w1;
            break;
        case G_SETOTHERMODE_H:
            gGfxRdp.otherModeH = mock_othermode(gGfxRdp.otherModeH, w0, w1);
            break;
        case G_SETOTHERMODE_L:
            gGfxRdp.otherModeL = mock_othermode(gGfxRdp.otherModeL, w0, w1);
            break;
        case G_SETSCISSOR:
            gGfxRdp.scissorUlx = (w0 >> 12) & 0xFFF;
            gGfxRdp.scissorUly = w0 & 0xFFF;
            gGfxRdp.scissorLrx = (w1 >> 12) & 0xFFF;
            gGfxRdp.scissorLry = w1 & 0xFFF;
            break;
        default:
            break;
    }
}

void gfx_rdp_texrect(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip) {
    if (gTexrectCount < MAX_TEXRECTS) {
        TexrectRec* r = &gTexrects[gTexrectCount];

        r->w0 = w0;
        r->w1 = w1;
        r->half1 = half1;
        r->half2 = half2;
        r->flip = flip;
    }
    gTexrectCount++;
}

void gfx_gx_image_rect(float ulx, float uly, float lrx, float lry, const GfxTexBinding* b, float s, float t,
                       float dsdx, float dtdy) {
    if (!isfinite(ulx) || !isfinite(uly) || !isfinite(lrx) || !isfinite(lry) || !isfinite(s) || !isfinite(t) ||
        !isfinite(dsdx) || !isfinite(dtdy)) {
        gNonFinite++;
    }
    if (gImageRectCount < MAX_IMAGE_RECTS) {
        ImageRectRec* r = &gImageRects[gImageRectCount];

        r->ulx = ulx, r->uly = uly, r->lrx = lrx, r->lry = lry;
        r->s = s, r->t = t, r->dsdx = dsdx, r->dtdy = dtdy;
        r->b = *b;
    }
    gImageRectCount++;
}

/* Texture cache */

void gfx_tex_get_stats(GfxTexStats* out) {
    *out = gTexStats;
}

bool gfx_tex_bind_image(const void* addr, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height, uint16_t stride,
                        const void* tlut, bool tlutIA, bool linear, int texMap, GfxTexBinding* out) {
    if (gBindCount < MAX_BINDS) {
        BindRec* r = &gBinds[gBindCount];

        r->addr = addr, r->fmt = fmt, r->siz = siz, r->width = width, r->height = height, r->stride = stride;
        r->tlut = tlut, r->tlutIA = tlutIA, r->linear = linear, r->texMap = texMap;
    }
    gBindCount++;
    memset(out, 0, sizeof(*out));
    if (gBindFail) {
        return false;
    }
    out->valid = true;
    out->width = width;
    out->height = height;
    out->sShiftScale = out->tShiftScale = 1.0f;
    out->linear = linear;
    return true;
}

/* Framebuffer effects */

bool gfx_fb_ready(void) {
    return gFbReady;
}

void gfx_fb_sync_ram(const void* addr, uint32_t bytes) {
    gFbSyncCount++;
    gFbSyncAddr = addr;
    gFbSyncBytes = bytes;
}

bool gfx_fb_bind_image(const void* addr, uint16_t width, uint16_t height, uint16_t stride, bool linear, int texMap,
                       GfxBindImageFn fromRam, GfxTexBinding* out) {
    gFbBindCount++;
    gFbBindAddr = addr;
    gFbBindLinear = linear;
    gFbBindFromRam = fromRam;
    gFbBound = false;
    if (!gFbBindImage || width != GFX_N64_WIDTH || height != GFX_N64_HEIGHT || stride != width ||
        texMap != GX_TEXMAP0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->width = width;
    out->height = height;
    out->sShiftScale = out->tShiftScale = 1.0f;
    out->linear = linear;
    gFbBound = true;
    return true;
}

void gfx_fb_image_done(void) {
    gFbDoneCount++;
    if (gFbBound) {
        gFbBound = false;
        gFbDoneRects = gImageRectCount;
    }
}

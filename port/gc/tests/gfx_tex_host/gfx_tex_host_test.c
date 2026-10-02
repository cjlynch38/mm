/**
 * Host tests for the renderer's RDP and texture modules (port/gc/gfx/gfx_rdp.c, gfx_tex.c), built with the
 * host compiler against a stand-in <gccore.h>. The GX calls are recorded, the game's RAM is a 24 MB array.
 *
 * The reference is an RDP model written here from the N64 documentation (angrylion's RDP as the source of
 * truth): a 4 KB TMEM of 16-bit halves filled by LOADBLOCK/LOADTILE/LOADTLUT with the odd-line word swap,
 * the texel fetch for every format/size, the TLUT, and the s/t coordinate pipeline (shift, tile offset,
 * clamp, mirror, mask). Every texture the implementation binds is read back through an independent GX
 * texel decoder and compared, texel by texel, with the reference quantized to the GX format.
 *
 *   - formats: every N64 format/size MM uses, CI with RGBA16 and IA16 palettes, fallbacks
 *   - GX tiled layouts at sizes that are not block multiples (LOADTILE sub-rectangles)
 *   - TMEM bookkeeping: LOADBLOCK with dxt, dxt 0 (pre-swapped rows), line skipping, wrong dxt, the _4b
 *     macros, two textures (TEXEL0/TEXEL1), overlapping loads, odd row offsets, partial palettes, mipmaps
 *   - wrap/mirror/clamp/mask/shift/offset: GX sampling of the binding against the RDP's coordinates
 *   - texture and fill rectangle decoding (one-pixel strips for textures that repeat within a pixel), other
 *     modes (F3DEX2 encoding), colors, scissor, images
 *   - texture cache: hits, content changes, LRU eviction and GX syncs under memory pressure, RAM rewritten
 *     during a task
 *   - whole images bound from RAM (gfx_tex_bind_image): every format, palettes, strides, large images
 *
 *   make -C port/gc/tests/gfx_tex_host run
 */
#include "gfx_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================================================ */
/* Test harness                                                                                     */
/* ================================================================================================ */

static int sChecks, sFailures, sTestFailures;
static const char* sTestName = "";

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        sChecks++;                                                            \
        if (!(cond)) {                                                        \
            sFailures++;                                                      \
            if (sTestFailures++ < 12) {                                       \
                printf("  FAIL [%s] %s:%d: ", sTestName, __FILE__, __LINE__); \
                printf(__VA_ARGS__);                                          \
                printf("\n");                                                 \
            }                                                                 \
        }                                                                     \
    } while (0)

static void test_begin(const char* name) {
    sTestName = name;
    sTestFailures = 0;
}

static void test_end(void) {
    printf("%-34s %s\n", sTestName, sTestFailures ? "FAILED" : "ok");
}

static uint32_t sRng = 0x12345678;
static uint32_t rnd(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

/* ================================================================================================ */
/* Stubs: RAM, segments, bridge, gfx_gx, GX                                                         */
/* ================================================================================================ */

#define RAM_SIZE (24u << 20)
static uint8_t* gRam;
static uint32_t gRamNext = 0x100000;

uint32_t gGfxSegments[16];

void* gfx_addr(uint32_t addr) {
    uint32_t phys;

    if (addr == 0) {
        return NULL;
    }
    if (addr & 0x80000000) {
        phys = addr & 0x1FFFFFFF;
    } else {
        phys = gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF);
    }
    phys &= 0x1FFFFFFF;
    if (phys >= RAM_SIZE) {
        printf("gfx_addr(%08X): outside the test RAM\n", addr);
        abort();
    }
    return gRam + phys;
}

/* RAM for test data: returns a physical address, 8-byte aligned */
static uint32_t ram_alloc(uint32_t size) {
    uint32_t a = gRamNext;

    gRamNext = (gRamNext + size + 63) & ~63u;
    if (gRamNext > RAM_SIZE) {
        printf("test RAM exhausted\n");
        abort();
    }
    return a;
}

static uint32_t ram_random(uint32_t size) {
    uint32_t a = ram_alloc(size), i;

    for (i = 0; i < size; i++) {
        gRam[a + i] = rnd() >> 24;
    }
    return a;
}

#define K0(phys) (0x80000000u | (phys))

static int gLogs;
void gc_log(const char* fmt, ...) {
    va_list ap;

    gLogs++;
    printf("    log: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static uint32_t gAllocLimit = 0xFFFFFFFF;
static int gAllocCalls;
void* gc_mem_alloc(unsigned int size, unsigned int align) {
    gAllocCalls++;
    if (size > gAllocLimit) {
        return NULL;
    }
    return aligned_alloc(align, (size + align - 1) / align * align);
}

static int gGxFlushes;
void gfx_gx_flush(void) {
    gGxFlushes++;
}

typedef struct {
    int calls;
    float ulx, uly, lrx, lry, s, t, dsdx, dtdy;
    int tile;
    bool flip;
} TexRectCall;

static TexRectCall gTexRect;          /* the last call, and the number of calls */
static TexRectCall gTexRectLog[64];   /* the first calls since the count was reset */

void gfx_gx_texrect(float ulx, float uly, float lrx, float lry, int tile, float s, float t, float dsdx, float dtdy,
                    bool flip) {
    if (gTexRect.calls >= 0 && gTexRect.calls < 64) {
        TexRectCall* c = &gTexRectLog[gTexRect.calls];

        c->ulx = ulx;
        c->uly = uly;
        c->lrx = lrx;
        c->lry = lry;
        c->tile = tile;
        c->s = s;
        c->t = t;
        c->dsdx = dsdx;
        c->dtdy = dtdy;
        c->flip = flip;
    }
    gTexRect.calls++;
    gTexRect.ulx = ulx;
    gTexRect.uly = uly;
    gTexRect.lrx = lrx;
    gTexRect.lry = lry;
    gTexRect.tile = tile;
    gTexRect.s = s;
    gTexRect.t = t;
    gTexRect.dsdx = dsdx;
    gTexRect.dtdy = dtdy;
    gTexRect.flip = flip;
}

static struct {
    int calls;
    float ulx, uly, lrx, lry;
} gFillRect;

void gfx_gx_fillrect(float ulx, float uly, float lrx, float lry) {
    gFillRect.calls++;
    gFillRect.ulx = ulx;
    gFillRect.uly = uly;
    gFillRect.lrx = lrx;
    gFillRect.lry = lry;
}

typedef struct {
    uint8_t* img;
    uint16_t w, h;
    uint8_t fmt, wrapS, wrapT, minf, magf;
} StubTex;
_Static_assert(sizeof(StubTex) <= sizeof(GXTexObj), "StubTex lives in a GXTexObj");

static StubTex gLoaded[8];
static int gInvalidates, gDrawDones, gTexLoads, gFlushedRanges;
static bool gDirtySinceInvalidate;
static int gFlushesAtLastDrawDone;
static int gGxErrors;

void GX_InitTexObj(GXTexObj* obj, void* img, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap) {
    StubTex st;

    memset(obj, 0, sizeof(*obj));
    st.img = img;
    st.w = wd;
    st.h = ht;
    st.fmt = fmt;
    st.wrapS = wrap_s;
    st.wrapT = wrap_t;
    st.minf = st.magf = GX_LINEAR;
    if (((uintptr_t)img & 31) != 0 || wd == 0 || ht == 0 || wd > 1024 || ht > 1024 || mipmap) {
        gGxErrors++;
    }
    if ((wrap_s != GX_CLAMP && (wd & (wd - 1))) || (wrap_t != GX_CLAMP && (ht & (ht - 1)))) {
        gGxErrors++; /* GX repeat/mirror needs a power-of-two size */
    }
    memcpy(obj, &st, sizeof(st));
}

void GX_InitTexObjFilterMode(GXTexObj* obj, u8 minfilt, u8 magfilt) {
    StubTex st;

    memcpy(&st, obj, sizeof(st));
    st.minf = minfilt;
    st.magf = magfilt;
    memcpy(obj, &st, sizeof(st));
}

void GX_LoadTexObj(GXTexObj* obj, u8 mapid) {
    memcpy(&gLoaded[mapid & 7], obj, sizeof(StubTex));
    gTexLoads++;
    if (gDirtySinceInvalidate) {
        gGxErrors++; /* converted texture memory loaded without invalidating the GX texture cache */
    }
}

void GX_InvalidateTexAll(void) {
    gInvalidates++;
    gDirtySinceInvalidate = false;
}

void GX_DrawDone(void) {
    gDrawDones++;
    if (gGxFlushes == gFlushesAtLastDrawDone) {
        gGxErrors++; /* pending draws not flushed before waiting */
    }
    gFlushesAtLastDrawDone = gGxFlushes;
}

void DCFlushRange(void* p, u32 len) {
    gFlushedRanges++;
    gDirtySinceInvalidate = true;
    if (((uintptr_t)p & 31) != 0 || (len & 31) != 0) {
        gGxErrors++;
    }
}

/* ================================================================================================ */
/* Reference RDP model                                                                              */
/* ================================================================================================ */

typedef struct {
    int fmt, siz, line, tmem, pal, cms, cmt, masks, maskt, shifts, shiftt;
    int sl, tl, sh, th;
} RefTile;

static struct {
    uint16_t tmem[2048]; /* TMEM as 16-bit halves: index = byte address / 2 */
    uint32_t timg;       /* physical */
    int tfmt, tsiz, twidth;
    RefTile tile[8];
    uint32_t omh;
} R;

static void ref_reset(void) {
    memset(&R, 0, sizeof(R));
}

static uint32_t ref_phys(uint32_t addr) {
    if (addr & 0x80000000) {
        return addr & 0x1FFFFFFF;
    }
    return (gGfxSegments[(addr >> 24) & 0xF] + (addr & 0xFFFFFF)) & 0x1FFFFFFF;
}

static uint8_t ref_tmem_byte(int b) {
    uint16_t v = R.tmem[(b & 0xFFF) >> 1];

    return (b & 1) ? (v & 0xFF) : (v >> 8);
}

static int ref_bytes(int siz) {
    return siz == 1 ? 1 : siz == 2 ? 2 : 4;
}

/* LOADBLOCK: one span of 64-bit words; the t counter advances by dxt per word (1.11), the TMEM address is
 * tmem + line * t + s, and words on odd t have their 32-bit halves exchanged (16-bit index ^ 2). */
static void ref_load_block(int tile, int sl, int tl, int sh, int dxt) {
    RefTile* t = &R.tile[tile];
    int bpt = ref_bytes(R.tsiz);
    int words = ((sh - sl + 1) * bpt + 7) / 8;
    uint32_t src = R.timg + (tl * R.twidth + sl) * bpt;
    int i, k;

    for (i = 0; i < words; i++) {
        int tt = (i * dxt) >> 11;
        int tbase = ((t->line * tt) & 0x1FF) + t->tmem;
        const uint8_t* w = gRam + src + i * 8;

        if (R.tsiz == 3) {
            for (k = 0; k < 2; k++) {
                int idx = ((tbase * 4 + i * 2 + k) ^ ((tt & 1) ? 2 : 0)) & 0x3FF;

                R.tmem[idx] = (w[k * 4] << 8) | w[k * 4 + 1];
                R.tmem[idx | 0x400] = (w[k * 4 + 2] << 8) | w[k * 4 + 3];
            }
        } else {
            for (k = 0; k < 4; k++) {
                int idx = ((tbase * 4 + i * 4 + k) ^ ((tt & 1) ? 2 : 0)) & 0x7FF;

                R.tmem[idx] = (w[k * 2] << 8) | w[k * 2 + 1];
            }
        }
    }
}

/* LOADTILE: rows tl..th of the texture image, row r (relative) at tmem + line * r, odd rows swapped */
static void ref_load_tile(int tile, int sl, int tl, int sh, int th) {
    RefTile* t = &R.tile[tile];
    int bpt = ref_bytes(R.tsiz);
    int cols = (sh >> 2) - (sl >> 2) + 1;
    int words = (cols * bpt + 7) / 8;
    int r, i, k;

    for (r = 0; r <= (th >> 2) - (tl >> 2); r++) {
        uint32_t src = R.timg + (((tl >> 2) + r) * R.twidth + (sl >> 2)) * bpt;
        int tbase = ((t->line * r) & 0x1FF) + t->tmem;

        for (i = 0; i < words; i++) {
            const uint8_t* w = gRam + src + i * 8;

            if (R.tsiz == 3) {
                for (k = 0; k < 2; k++) {
                    int idx = ((tbase * 4 + i * 2 + k) ^ ((r & 1) ? 2 : 0)) & 0x3FF;

                    R.tmem[idx] = (w[k * 4] << 8) | w[k * 4 + 1];
                    R.tmem[idx | 0x400] = (w[k * 4 + 2] << 8) | w[k * 4 + 3];
                }
            } else {
                for (k = 0; k < 4; k++) {
                    int idx = ((tbase * 4 + i * 4 + k) ^ ((r & 1) ? 2 : 0)) & 0x7FF;

                    R.tmem[idx] = (w[k * 2] << 8) | w[k * 2 + 1];
                }
            }
        }
    }
}

/* LOADTLUT: entry e fills TMEM word tmem + e (all four halves) */
static void ref_load_tlut(int tile, int sl, int tl, int sh, int th) {
    RefTile* t = &R.tile[tile];
    int n = (sh >> 2) - (sl >> 2) + 1, e, k;
    uint32_t src = R.timg + ((tl >> 2) * R.twidth + (sl >> 2)) * 2;

    for (e = 0; e < n; e++) {
        uint16_t v = (gRam[src + e * 2] << 8) | gRam[src + e * 2 + 1];

        for (k = 0; k < 4; k++) {
            R.tmem[((t->tmem + e) * 4 + k) & 0x7FF] = v;
        }
    }
}

static void ref_cmd(uint32_t w0, uint32_t w1) {
    int op = w0 >> 24, tile = (w1 >> 24) & 7;
    RefTile* t = &R.tile[tile];

    switch (op) {
        case G_SETTIMG:
            R.timg = ref_phys(w1);
            R.tfmt = (w0 >> 21) & 7;
            R.tsiz = (w0 >> 19) & 3;
            R.twidth = (w0 & 0x3FF) + 1;
            break;
        case G_SETTILE:
            t->fmt = (w0 >> 21) & 7;
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 0x1FF;
            t->tmem = w0 & 0x1FF;
            t->pal = (w1 >> 20) & 0xF;
            t->cmt = (w1 >> 18) & 3;
            t->maskt = (w1 >> 14) & 0xF;
            t->shiftt = (w1 >> 10) & 0xF;
            t->cms = (w1 >> 8) & 3;
            t->masks = (w1 >> 4) & 0xF;
            t->shifts = w1 & 0xF;
            break;
        case G_SETTILESIZE:
        case G_LOADBLOCK:
        case G_LOADTILE:
        case G_LOADTLUT:
            t->sl = (w0 >> 12) & 0xFFF;
            t->tl = w0 & 0xFFF;
            t->sh = (w1 >> 12) & 0xFFF;
            t->th = w1 & 0xFFF;
            if (op == G_LOADBLOCK) {
                ref_load_block(tile, t->sl, t->tl, t->sh, t->th);
            } else if (op == G_LOADTILE) {
                ref_load_tile(tile, t->sl, t->tl, t->sh, t->th);
            } else if (op == G_LOADTLUT) {
                ref_load_tlut(tile, t->sl, t->tl, t->sh, t->th);
            }
            break;
        case G_RDPSETOTHERMODE:
            R.omh = w0 & 0xFFFFFF;
            break;
        case G_SETOTHERMODE_H: {
            int len = (w0 & 0xFF) + 1, sft = 32 - ((w0 >> 8) & 0xFF) - len;
            uint32_t mask = (len == 32) ? 0xFFFFFFFF : ((1u << len) - 1) << sft;

            R.omh = (R.omh & ~mask) | w1;
            break;
        }
    }
}

static void decode_rgba16(uint16_t c, uint8_t o[4]) {
    int r = c >> 11, g = (c >> 6) & 31, b = (c >> 1) & 31;

    o[0] = (r << 3) | (r >> 2);
    o[1] = (g << 3) | (g >> 2);
    o[2] = (b << 3) | (b >> 2);
    o[3] = (c & 1) ? 255 : 0;
}

static void decode_ia16(uint16_t c, uint8_t o[4]) {
    o[0] = o[1] = o[2] = c >> 8;
    o[3] = c & 0xFF;
}

static void ref_palette(int index, uint8_t o[4]) {
    uint16_t c = R.tmem[((0x100 + index) * 4) & 0x7FF];

    if (R.omh & (1 << 14)) {
        decode_ia16(c, o);
    } else {
        decode_rgba16(c, o);
    }
}

/* Texel (s, t) of a tile, relative to the tile (after clamp/mirror/mask), as RGBA8 */
static void ref_fetch(int tile, int s, int t, uint8_t o[4]) {
    const RefTile* tl = &R.tile[tile];
    int tbase = tl->line * (t & 0xFF) + tl->tmem;
    bool odd = t & 1;
    bool tlut = (R.omh >> 15) & 1;
    int v, b;

    switch (tl->siz) {
        case 0: /* with the TLUT on, texel addresses wrap in the low 2 KB */
            b = ((((tbase << 4) + s) >> 1) ^ (odd ? 4 : 0)) & (tlut ? 0x7FF : 0xFFF);
            v = ref_tmem_byte(b);
            v = (s & 1) ? (v & 0xF) : (v >> 4);
            if (tlut) {
                ref_palette((tl->pal << 4) | v, o);
            } else if (tl->fmt == G_IM_FMT_IA) {
                int i = v >> 1;

                o[0] = o[1] = o[2] = (i << 5) | (i << 2) | (i >> 1);
                o[3] = (v & 1) ? 255 : 0;
            } else if (tl->fmt == G_IM_FMT_CI) {
                o[0] = o[1] = o[2] = o[3] = (tl->pal << 4) | v;
            } else {
                o[0] = o[1] = o[2] = o[3] = v * 17;
            }
            break;
        case 1:
            b = (((tbase << 3) + s) ^ (odd ? 4 : 0)) & (tlut ? 0x7FF : 0xFFF);
            v = ref_tmem_byte(b);
            if (tlut) {
                ref_palette(v, o);
            } else if (tl->fmt == G_IM_FMT_IA) {
                o[0] = o[1] = o[2] = (v >> 4) * 17;
                o[3] = (v & 0xF) * 17;
            } else {
                o[0] = o[1] = o[2] = o[3] = v;
            }
            break;
        case 2:
            v = R.tmem[(((tbase << 2) + s) ^ (odd ? 2 : 0)) & 0x7FF];
            if (tl->fmt == G_IM_FMT_RGBA) {
                decode_rgba16(v, o);
            } else {
                decode_ia16(v, o);
            }
            break;
        default: {
            int idx = (((tbase << 2) + s) ^ (odd ? 2 : 0)) & 0x3FF;

            o[0] = R.tmem[idx] >> 8;
            o[1] = R.tmem[idx] & 0xFF;
            o[2] = R.tmem[idx | 0x400] >> 8;
            o[3] = R.tmem[idx | 0x400] & 0xFF;
            break;
        }
    }
}

/* The RDP's clamp, mirror and mask of one coordinate (integer texel relative to the tile) */
static int ref_clamp_mask(int si, int sl, int sh, int mask, int cm) {
    int clampMax = ((sh >> 2) - (sl >> 2)) & 0x3FF;

    if ((cm & G_TX_CLAMP) || mask == 0) {
        if (si < 0) {
            si = 0;
        } else if (si > clampMax) {
            si = clampMax;
        }
    }
    if (mask != 0) {
        int m = mask > 10 ? 10 : mask;

        if ((cm & G_TX_MIRROR) && (si & (1 << m))) {
            si = ~si;
        }
        si &= (1 << m) - 1;
    }
    return si;
}

/* The clamp bit as the RDP applies it: COPY mode only masks (angrylion tc_pipeline_copy) */
static int ref_cm(int cm, int mask) {
    return (((R.omh >> G_MDSFT_CYCLETYPE) & 3) == 2 && mask != 0) ? (cm & ~G_TX_CLAMP) : cm;
}

/* s10.5 vertex coordinate -> texel relative to the tile, through shift and tile offset */
static int ref_coord(int32_t s105, int shift, int sl) {
    int32_t s = (shift <= 10) ? (s105 >> shift) : (int32_t)((uint32_t)s105 << (16 - shift));

    s -= sl << 3;
    return s >> 5; /* floor */
}

/* ================================================================================================ */
/* GX: quantization of a reference color to a GX format, and an independent texel decoder           */
/* ================================================================================================ */

static void gx_quantize(int fmt, const uint8_t in[4], uint8_t o[4]) {
    int v, a;

    switch (fmt) {
        case GX_TF_I4:
            v = (in[0] + 8) / 17 * 17;
            o[0] = o[1] = o[2] = o[3] = v;
            break;
        case GX_TF_I8:
            o[0] = o[1] = o[2] = o[3] = in[0];
            break;
        case GX_TF_IA4:
            o[0] = o[1] = o[2] = (in[0] + 8) / 17 * 17;
            o[3] = (in[3] + 8) / 17 * 17;
            break;
        case GX_TF_IA8:
            o[0] = o[1] = o[2] = in[0];
            o[3] = in[3];
            break;
        case GX_TF_RGB5A3:
            if (in[3] == 255) {
                for (v = 0; v < 3; v++) {
                    o[v] = (in[v] & 0xF8) | (in[v] >> 5);
                }
                o[3] = 255;
            } else {
                for (v = 0; v < 3; v++) {
                    o[v] = (in[v] >> 4) * 17;
                }
                a = in[3] >> 5;
                o[3] = (a << 5) | (a << 2) | (a >> 1);
            }
            break;
        default:
            memcpy(o, in, 4);
            break;
    }
}

static void gx_texel(const StubTex* st, int x, int y, uint8_t o[4]) {
    int bw = (st->fmt == GX_TF_I4 || st->fmt == GX_TF_I8 || st->fmt == GX_TF_IA4) ? 8 : 4;
    int bh = (st->fmt == GX_TF_I4) ? 8 : 4;
    int bsz = (st->fmt == GX_TF_RGBA8) ? 64 : 32;
    int blocksPerRow = (st->w + bw - 1) / bw;
    const uint8_t* blk = st->img + ((y / bh) * blocksPerRow + (x / bw)) * bsz;
    int ix = x % bw, iy = y % bh, v, a;
    const uint8_t* p;

    switch (st->fmt) {
        case GX_TF_I4:
            v = blk[iy * 4 + ix / 2];
            v = ((ix & 1) ? (v & 0xF) : (v >> 4)) * 17;
            o[0] = o[1] = o[2] = o[3] = v;
            break;
        case GX_TF_I8:
            o[0] = o[1] = o[2] = o[3] = blk[iy * 8 + ix];
            break;
        case GX_TF_IA4:
            v = blk[iy * 8 + ix];
            o[0] = o[1] = o[2] = (v & 0xF) * 17;
            o[3] = (v >> 4) * 17;
            break;
        case GX_TF_IA8:
            p = blk + (iy * 4 + ix) * 2;
            o[0] = o[1] = o[2] = p[1];
            o[3] = p[0];
            break;
        case GX_TF_RGB5A3:
            p = blk + (iy * 4 + ix) * 2;
            v = (p[0] << 8) | p[1];
            if (v & 0x8000) {
                int r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;

                o[0] = (r << 3) | (r >> 2);
                o[1] = (g << 3) | (g >> 2);
                o[2] = (b << 3) | (b >> 2);
                o[3] = 255;
            } else {
                a = (v >> 12) & 7;
                o[0] = ((v >> 8) & 15) * 17;
                o[1] = ((v >> 4) & 15) * 17;
                o[2] = (v & 15) * 17;
                o[3] = (a << 5) | (a << 2) | (a >> 1);
            }
            break;
        default: /* RGBA8 */
            p = blk + (iy * 4 + ix) * 2;
            o[3] = p[0];
            o[0] = p[1];
            o[1] = p[32];
            o[2] = p[33];
            break;
    }
}

/* ================================================================================================ */
/* Display list commands (encodings from include/PR/gbi.h), sent to the implementation and the model */
/* ================================================================================================ */

#define SHIFTL(v, s, w) ((((uint32_t)(v)) & ((1u << (w)) - 1)) << (s))

static uint32_t sCmdLog[256][2];
static int sCmdLogN;

static void emit(uint32_t w0, uint32_t w1) {
    if (sCmdLogN < 256) {
        sCmdLog[sCmdLogN][0] = w0;
        sCmdLog[sCmdLogN][1] = w1;
        sCmdLogN++;
    }
    ref_cmd(w0, w1);
    gfx_rdp_command(w0, w1);
}

static void dp_sync(int op) {
    emit(SHIFTL(op, 24, 8), 0);
}

static void dp_set_timg(int fmt, int siz, int width, uint32_t addr) {
    emit(SHIFTL(G_SETTIMG, 24, 8) | SHIFTL(fmt, 21, 3) | SHIFTL(siz, 19, 2) | SHIFTL(width - 1, 0, 12), addr);
}

static void dp_set_tile(int fmt, int siz, int line, int tmem, int tile, int pal, int cmt, int maskt, int shiftt,
                        int cms, int masks, int shifts) {
    emit(SHIFTL(G_SETTILE, 24, 8) | SHIFTL(fmt, 21, 3) | SHIFTL(siz, 19, 2) | SHIFTL(line, 9, 9) | SHIFTL(tmem, 0, 9),
         SHIFTL(tile, 24, 3) | SHIFTL(pal, 20, 4) | SHIFTL(cmt, 18, 2) | SHIFTL(maskt, 14, 4) | SHIFTL(shiftt, 10, 4) |
             SHIFTL(cms, 8, 2) | SHIFTL(masks, 4, 4) | SHIFTL(shifts, 0, 4));
}

static void dp_tile_generic(int op, int tile, int uls, int ult, int lrs, int lrt) {
    emit(SHIFTL(op, 24, 8) | SHIFTL(uls, 12, 12) | SHIFTL(ult, 0, 12),
         SHIFTL(tile, 24, 3) | SHIFTL(lrs, 12, 12) | SHIFTL(lrt, 0, 12));
}

static void dp_set_tile_size(int tile, int uls, int ult, int lrs, int lrt) {
    dp_tile_generic(G_SETTILESIZE, tile, uls, ult, lrs, lrt);
}

static void dp_load_tile(int tile, int uls, int ult, int lrs, int lrt) {
    dp_tile_generic(G_LOADTILE, tile, uls, ult, lrs, lrt);
}

static void dp_load_block(int tile, int uls, int ult, int lrs, int dxt) {
    dp_tile_generic(G_LOADBLOCK, tile, uls, ult, lrs > 2047 ? 2047 : lrs, dxt);
}

static void dp_load_tlut_cmd(int tile, int count) {
    emit(SHIFTL(G_LOADTLUT, 24, 8), SHIFTL(tile, 24, 3) | SHIFTL(count, 14, 10));
}

static void sp_othermode(int cmd, int sft, int len, uint32_t data) {
    emit(SHIFTL(cmd, 24, 8) | SHIFTL(32 - sft - len, 8, 8) | SHIFTL(len - 1, 0, 8), data);
}

static void dp_set_tlut(uint32_t type) {
    sp_othermode(G_SETOTHERMODE_H, G_MDSFT_TEXTLUT, 2, type);
}

static void dp_set_cycle(uint32_t type) {
    sp_othermode(G_SETOTHERMODE_H, G_MDSFT_CYCLETYPE, 2, type);
}

static void dp_set_filter(uint32_t type) {
    sp_othermode(G_SETOTHERMODE_H, G_MDSFT_TEXTFILT, 2, type);
}

/* siz##_LOAD_BLOCK, _INCR, _SHIFT, _BYTES, _LINE_BYTES, _TILE_BYTES for G_IM_SIZ_4b..32b */
static const int kLoadBlock[4] = { 2, 2, 2, 3 };
static const int kIncr[4] = { 3, 1, 0, 0 };
static const int kShift[4] = { 2, 1, 0, 0 };
static const int kBytes[4] = { 0, 1, 2, 4 };
static const int kLineBytes[4] = { 0, 1, 2, 2 };
static const int kTileBytes[4] = { 0, 1, 2, 2 };

static int txl2words(int txls, int b) {
    int w = txls * b / 8;

    return w > 1 ? w : 1;
}

static int calc_dxt(int width, int b) {
    return ((1 << 11) + txl2words(width, b) - 1) / txl2words(width, b);
}

static int calc_dxt_4b(int width) {
    int w = width / 16 > 1 ? width / 16 : 1;

    return ((1 << 11) + w - 1) / w;
}

/* gDPLoadMultiBlock / gDPLoadTextureBlock (tmem 0, render tile 0); dxt0: the ...S variants */
static void load_multi_block(uint32_t timg, int tmem, int rtile, int fmt, int siz, int w, int h, int pal, int cms,
                             int cmt, int masks, int maskt, int shifts, int shiftt, bool dxt0) {
    dp_set_timg(fmt, kLoadBlock[siz], 1, timg);
    dp_set_tile(fmt, kLoadBlock[siz], 0, tmem, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts);
    dp_sync(G_RDPLOADSYNC);
    dp_load_block(G_TX_LOADTILE, 0, 0, ((w * h + kIncr[siz]) >> kShift[siz]) - 1, dxt0 ? 0 : calc_dxt(w, kBytes[siz]));
    dp_sync(G_RDPPIPESYNC);
    dp_set_tile(fmt, siz, ((w * kLineBytes[siz]) + 7) >> 3, tmem, rtile, pal, cmt, maskt, shiftt, cms, masks, shifts);
    dp_set_tile_size(rtile, 0, 0, (w - 1) << 2, (h - 1) << 2);
}

static void load_texture_block(uint32_t timg, int fmt, int siz, int w, int h, int pal, int cms, int cmt, int masks,
                               int maskt, int shifts, int shiftt) {
    load_multi_block(timg, 0, 0, fmt, siz, w, h, pal, cms, cmt, masks, maskt, shifts, shiftt, false);
}

/* gDPLoadMultiBlock_4b / gDPLoadTextureBlock_4b: 4-bit textures loaded as 16-bit blocks */
static void load_multi_block_4b(uint32_t timg, int tmem, int rtile, int fmt, int w, int h, int pal, int cms, int cmt,
                                int masks, int maskt, int shifts, int shiftt) {
    dp_set_timg(fmt, G_IM_SIZ_16b, 1, timg);
    dp_set_tile(fmt, G_IM_SIZ_16b, 0, tmem, G_TX_LOADTILE, 0, cmt, maskt, shiftt, cms, masks, shifts);
    dp_sync(G_RDPLOADSYNC);
    dp_load_block(G_TX_LOADTILE, 0, 0, ((w * h + 3) >> 2) - 1, calc_dxt_4b(w));
    dp_sync(G_RDPPIPESYNC);
    dp_set_tile(fmt, G_IM_SIZ_4b, (((w) >> 1) + 7) >> 3, tmem, rtile, pal, cmt, maskt, shiftt, cms, masks, shifts);
    dp_set_tile_size(rtile, 0, 0, (w - 1) << 2, (h - 1) << 2);
}

/* gDPLoadMultiTile / gDPLoadTextureTile */
static void load_multi_tile(uint32_t timg, int tmem, int rtile, int fmt, int siz, int width, int uls, int ult, int lrs,
                            int lrt, int pal, int cms, int cmt, int masks, int maskt, int shifts, int shiftt) {
    dp_set_timg(fmt, siz, width, timg);
    dp_set_tile(fmt, siz, ((((lrs) - (uls) + 1) * kTileBytes[siz]) + 7) >> 3, tmem, G_TX_LOADTILE, 0, cmt, maskt,
                shiftt, cms, masks, shifts);
    dp_sync(G_RDPLOADSYNC);
    dp_load_tile(G_TX_LOADTILE, uls << 2, ult << 2, lrs << 2, lrt << 2);
    dp_sync(G_RDPPIPESYNC);
    dp_set_tile(fmt, siz, ((((lrs) - (uls) + 1) * kLineBytes[siz]) + 7) >> 3, tmem, rtile, pal, cmt, maskt, shiftt, cms,
                masks, shifts);
    dp_set_tile_size(rtile, uls << 2, ult << 2, lrs << 2, lrt << 2);
}

/* gDPLoadMultiTile_4b / gDPLoadTextureTile_4b: loaded as 8-bit texels */
static void load_multi_tile_4b(uint32_t timg, int tmem, int rtile, int fmt, int width, int uls, int ult, int lrs,
                               int lrt, int pal, int cms, int cmt, int masks, int maskt, int shifts, int shiftt) {
    dp_set_timg(fmt, G_IM_SIZ_8b, width >> 1, timg);
    dp_set_tile(fmt, G_IM_SIZ_8b, ((((lrs) - (uls) + 1) >> 1) + 7) >> 3, tmem, G_TX_LOADTILE, 0, cmt, maskt, shiftt,
                cms, masks, shifts);
    dp_sync(G_RDPLOADSYNC);
    dp_load_tile(G_TX_LOADTILE, uls << 1, ult << 2, lrs << 1, lrt << 2);
    dp_sync(G_RDPPIPESYNC);
    dp_set_tile(fmt, G_IM_SIZ_4b, ((((lrs) - (uls) + 1) >> 1) + 7) >> 3, tmem, rtile, pal, cmt, maskt, shiftt, cms,
                masks, shifts);
    dp_set_tile_size(rtile, uls << 2, ult << 2, lrs << 2, lrt << 2);
}

/* gDPLoadTLUT(count, tmemaddr, dram); pal16 = (16, 256 + pal * 16), pal256 = (256, 256) */
static void load_tlut(int count, int tmemaddr, uint32_t dram) {
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, dram);
    dp_sync(G_RDPTILESYNC);
    dp_set_tile(0, 0, 0, tmemaddr, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    dp_sync(G_RDPLOADSYNC);
    dp_load_tlut_cmd(G_TX_LOADTILE, count - 1);
    dp_sync(G_RDPPIPESYNC);
}

/* Start of a task: the implementation clears its RDP state and TMEM records; so does the model */
static void task_begin(void) {
    gfx_rdp_reset();
    gfx_tex_frame();
    ref_reset();
    R.omh = gGfxRdp.otherModeH;
}

/* ================================================================================================ */
/* Comparisons                                                                                      */
/* ================================================================================================ */

static int sCompareErrors;

static uint32_t slow_binds(void) {
    GfxTexStats st;

    gfx_tex_get_stats(&st);
    return st.slow;
}

/* Bind `tile` and compare every texel of the GX texture with the model: GX texel (X, Y) is the RDP's tile
 * coordinate (X, Y) before clamp/mirror/mask */
static bool compare_tile(int tile, int texMap, int expectFmt, GfxTexBinding* outB) {
    GfxTexBinding b;
    const RefTile* rt = &R.tile[tile];
    const StubTex* st;
    int x, y, errors = 0;

    memset(&b, 0, sizeof(b));
    if (!gfx_tex_bind(tile, texMap, &b) || !b.valid) {
        CHECK(false, "tile %d: bind failed", tile);
        return false;
    }
    st = &gLoaded[texMap];
    CHECK(st->w == b.width && st->h == b.height, "tile %d: binding %ux%u vs GX %ux%u", tile, b.width, b.height, st->w,
          st->h);
    if (expectFmt >= 0) {
        CHECK(st->fmt == expectFmt, "tile %d: GX format %d, expected %d", tile, st->fmt, expectFmt);
    }
    for (y = 0; y < st->h; y++) {
        for (x = 0; x < st->w; x++) {
            uint8_t ref[4], q[4], got[4];
            int s = ref_clamp_mask(x, rt->sl, rt->sh, rt->masks, ref_cm(rt->cms, rt->masks));
            int t = ref_clamp_mask(y, rt->tl, rt->th, rt->maskt, ref_cm(rt->cmt, rt->maskt));

            ref_fetch(tile, s, t, ref);
            gx_quantize(st->fmt, ref, q);
            gx_texel(st, x, y, got);
            if (memcmp(q, got, 4) != 0) {
                if (errors++ < 4) {
                    CHECK(false, "tile %d texel (%d,%d): got %02X%02X%02X%02X expected %02X%02X%02X%02X", tile, x, y,
                          got[0], got[1], got[2], got[3], q[0], q[1], q[2], q[3]);
                }
            }
        }
    }
    sCompareErrors += errors;
    CHECK(errors == 0, "tile %d: %d texels differ (%ux%u)", tile, errors, st->w, st->h);
    if (outB != NULL) {
        *outB = b;
    }
    return errors == 0;
}

/* Sample the bound texture like GX does (point sampling, the binding's wrap modes) at vertex coordinates
 * s10.5 = S, T and compare with the RDP's coordinate pipeline on the model */
static void compare_sampling(int tile, int texMap) {
    GfxTexBinding b;
    const RefTile* rt = &R.tile[tile];
    const StubTex* st;
    int S, T, samples = 0, errors = 0;
    double ss = rt->shifts == 0 ? 1.0 : rt->shifts <= 10 ? 1.0 / (1 << rt->shifts) : (double)(1 << (16 - rt->shifts));
    double ts = rt->shiftt == 0 ? 1.0 : rt->shiftt <= 10 ? 1.0 / (1 << rt->shiftt) : (double)(1 << (16 - rt->shiftt));

    if (!gfx_tex_bind(tile, texMap, &b) || !b.valid) {
        CHECK(false, "tile %d: bind failed", tile);
        return;
    }
    st = &gLoaded[texMap];
    CHECK(!b.linear, "point sampling expected");
    CHECK(b.sShiftScale != 0.0f && b.tShiftScale != 0.0f, "zero shift scale");
    for (T = -3000; T < 3000; T += 37) {
        double tpos = T / 32.0 * ts - rt->tl / 4.0;

        if (fabs(tpos - floor(tpos + 0.5)) < 0.07) {
            continue;
        }
        for (S = -3000; S < 3000; S += 23) {
            double spos = S / 32.0 * ss - rt->sl / 4.0;
            float u, v;
            int x, y, w2, h2;
            uint8_t ref[4], q[4], got[4];

            if (fabs(spos - floor(spos + 0.5)) < 0.07) {
                continue;
            }
            /* RDP */
            ref_fetch(tile,
                      ref_clamp_mask(ref_coord(S, rt->shifts, rt->sl), rt->sl, rt->sh, rt->masks,
                                     ref_cm(rt->cms, rt->masks)),
                      ref_clamp_mask(ref_coord(T, rt->shiftt, rt->tl), rt->tl, rt->th, rt->maskt,
                                     ref_cm(rt->cmt, rt->maskt)),
                      ref);
            gx_quantize(st->fmt, ref, q);
            /* GX */
            gfx_tex_uv(&b, S / 32.0f, T / 32.0f, &u, &v);
            x = (int)floorf(u * st->w);
            y = (int)floorf(v * st->h);
            w2 = st->w * 2;
            h2 = st->h * 2;
            if (st->wrapS == GX_CLAMP) {
                x = x < 0 ? 0 : x >= st->w ? st->w - 1 : x;
            } else if (st->wrapS == GX_REPEAT) {
                x = ((x % st->w) + st->w) % st->w;
            } else {
                x = ((x % w2) + w2) % w2;
                x = x >= st->w ? w2 - 1 - x : x;
            }
            if (st->wrapT == GX_CLAMP) {
                y = y < 0 ? 0 : y >= st->h ? st->h - 1 : y;
            } else if (st->wrapT == GX_REPEAT) {
                y = ((y % st->h) + st->h) % st->h;
            } else {
                y = ((y % h2) + h2) % h2;
                y = y >= st->h ? h2 - 1 - y : y;
            }
            gx_texel(st, x, y, got);
            samples++;
            if (memcmp(q, got, 4) != 0 && errors++ < 4) {
                CHECK(false, "tile %d sample s=%d t=%d (texel %.2f,%.2f): GX texel (%d,%d) differs", tile, S, T, spos,
                      tpos, x, y);
            }
        }
    }
    CHECK(errors == 0, "tile %d: %d of %d samples differ", tile, errors, samples);
    CHECK(samples > 1000, "too few samples (%d)", samples);
}

/* ================================================================================================ */
/* Tests: formats                                                                                   */
/* ================================================================================================ */

static void test_formats(void) {
    uint32_t a, pal;
    uint32_t slow0 = slow_binds();

    test_begin("format RGBA16 -> RGB5A3");
    task_begin();
    a = ram_random(32 * 32 * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("format RGBA32 -> RGBA8");
    task_begin();
    a = ram_random(16 * 16 * 4);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGBA8, NULL);
    test_end();

    test_begin("format IA4 -> IA4 (_4b block)");
    task_begin();
    a = ram_random(32 * 16 / 2);
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_IA, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA4, NULL);
    test_end();

    test_begin("format IA8 -> IA4");
    task_begin();
    a = ram_random(32 * 16);
    load_texture_block(K0(a), G_IM_FMT_IA, G_IM_SIZ_8b, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA4, NULL);
    test_end();

    test_begin("format IA16 -> IA8");
    task_begin();
    a = ram_random(16 * 16 * 2);
    load_texture_block(K0(a), G_IM_FMT_IA, G_IM_SIZ_16b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA8, NULL);
    test_end();

    test_begin("format I4 -> I4 (_4b block)");
    task_begin();
    a = ram_random(64 * 32 / 2);
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_I, 64, 32, 0, 0, 0, 6, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_I4, NULL);
    test_end();

    test_begin("format I8 -> I8");
    task_begin();
    a = ram_random(64 * 32);
    load_texture_block(K0(a), G_IM_FMT_I, G_IM_SIZ_8b, 64, 32, 0, 0, 0, 6, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    test_end();

    test_begin("format CI4 + RGBA16 TLUT (pal16)");
    task_begin();
    pal = ram_random(16 * 2);
    a = ram_random(32 * 32 / 2);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(16, 256 + 5 * 16, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 32, 32, 5, 0, 0, 5, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("format CI8 + RGBA16 TLUT (pal256)");
    task_begin();
    pal = ram_random(256 * 2);
    a = ram_random(32 * 32);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(256, 256, K0(pal));
    load_texture_block(K0(a), G_IM_FMT_CI, G_IM_SIZ_8b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("format CI4 + IA16 TLUT");
    task_begin();
    pal = ram_random(16 * 2);
    a = ram_random(16 * 16 / 2);
    dp_set_tlut(G_TT_IA16);
    load_tlut(16, 256 + 2 * 16, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 2, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA8, NULL);
    test_end();

    test_begin("format CI8 + IA16 TLUT");
    task_begin();
    pal = ram_random(256 * 2);
    a = ram_random(32 * 16);
    dp_set_tlut(G_TT_IA16);
    load_tlut(256, 256, K0(pal));
    load_texture_block(K0(a), G_IM_FMT_CI, G_IM_SIZ_8b, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA8, NULL);
    test_end();

    test_begin("format CI4 without TLUT -> I8");
    task_begin();
    a = ram_random(16 * 16 / 2);
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 9, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    test_end();

    test_begin("format RGBA 8b (read as I8)");
    task_begin();
    a = ram_random(16 * 16);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_8b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    test_end();

    test_begin("format I4 with TLUT on (palette index)");
    task_begin();
    pal = ram_random(16 * 2);
    a = ram_random(16 * 16 / 2);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(16, 256 + 7 * 16, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_I, 16, 16, 7, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("formats: gbi.h loads take the fast path");
    CHECK(slow_binds() == slow0, "%u slow binds", slow_binds() - slow0);
    test_end();
}

/* ================================================================================================ */
/* Tests: tiled layouts at odd sizes (LOADTILE sub-rectangles)                                      */
/* ================================================================================================ */

static void test_loadtile_layouts(void) {
    uint32_t a, pal;
    uint32_t slow0 = slow_binds();

    test_begin("loadtile RGBA16 12x6 of 40x20");
    task_begin();
    a = ram_random(40 * 20 * 2);
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 40, 4, 3, 15, 8, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    CHECK(gLoaded[0].w == 12 && gLoaded[0].h == 6, "size %dx%d", gLoaded[0].w, gLoaded[0].h);
    test_end();

    test_begin("loadtile I4 10x5 of 24x16 (_4b)");
    task_begin();
    a = ram_random(24 * 16 / 2);
    load_multi_tile_4b(K0(a), 0, 0, G_IM_FMT_I, 24, 6, 2, 15, 6, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_I4, NULL);
    CHECK(gLoaded[0].w == 10 && gLoaded[0].h == 5, "size %dx%d", gLoaded[0].w, gLoaded[0].h);
    test_end();

    test_begin("loadtile IA8 6x3 of 20x10");
    task_begin();
    a = ram_random(20 * 10);
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_IA, G_IM_SIZ_8b, 20, 3, 1, 8, 3, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_IA4, NULL);
    test_end();

    test_begin("loadtile RGBA32 5x3 of 9x7");
    task_begin();
    a = ram_random(9 * 7 * 4);
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_32b, 9, 2, 1, 6, 3, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGBA8, NULL);
    test_end();

    test_begin("loadtile CI8 7x9 of 30x30 + pal256");
    task_begin();
    pal = ram_random(512);
    a = ram_random(30 * 30);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(256, 256, K0(pal));
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_CI, G_IM_SIZ_8b, 30, 11, 5, 17, 13, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("loadtile 320x6 RGBA16 strip, segmented");
    task_begin();
    a = ram_random(320 * 240 * 2);
    gGfxSegments[0xF] = a;
    load_multi_tile(0x0F000000, 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 0, 120, 319, 125, 0, G_TX_CLAMP, G_TX_CLAMP, 0,
                    0, 0, 0);
    gGfxSegments[0xF] = 0x200000; /* segments may change after the load: the texture image was resolved */
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("title logo: RGBA32 strips (EnMag)");
    {
        /* EnMag_DrawImageRGBA32: gDPSetTileCustom once, then a LOADTILE of TMEM_SIZE / (width * 4) rows per strip */
        int width = 64, height = 40, rows = 4096 / (64 * 4), y0;

        task_begin();
        a = ram_random(width * height * 4);
        for (y0 = 0; y0 < height; y0 += rows) {
            int h = (height - y0 < rows) ? height - y0 : rows;

            dp_sync(G_RDPPIPESYNC);
            dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_32b, (width * 2 + 7) >> 3, 0, G_TX_LOADTILE, 0, G_TX_CLAMP, 0, 0,
                        G_TX_CLAMP, 0, 0);
            dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_32b, (width * 2 + 7) >> 3, 0, G_TX_RENDERTILE, 0, G_TX_CLAMP, 0, 0,
                        G_TX_CLAMP, 0, 0);
            dp_set_tile_size(G_TX_RENDERTILE, 0, 0, (width - 1) << 2, (h - 1) << 2);
            dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_32b, width, K0(a + y0 * width * 4));
            dp_sync(G_RDPLOADSYNC);
            dp_load_tile(G_TX_LOADTILE, 0, 0, (width - 1) << 2, (h - 1) << 2);
            compare_tile(0, GX_TEXMAP0, GX_TF_RGBA8, NULL);
            CHECK(gLoaded[0].w == width && gLoaded[0].h == h, "strip at %d: %dx%d", y0, gLoaded[0].w, gLoaded[0].h);
        }
    }
    test_end();

    test_begin("loadtiles take the fast path");
    CHECK(slow_binds() == slow0, "%u slow binds", slow_binds() - slow0);
    test_end();
}

/* ================================================================================================ */
/* Tests: TMEM bookkeeping                                                                          */
/* ================================================================================================ */

static void test_tmem(void) {
    uint32_t a, b, pal, slow0;
    int i, x, y;

    test_begin("loadblock dxt 0, rows pre-swapped");
    task_begin();
    {
        /* image 16x8 RGBA16: rows of 32 bytes; RAM holds odd rows with their 32-bit words swapped */
        uint8_t img[16 * 8 * 2];

        for (i = 0; i < (int)sizeof(img); i++) {
            img[i] = rnd() >> 24;
        }
        a = ram_alloc(sizeof(img));
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 32; x++) {
                gRam[a + y * 32 + x] = img[y * 32 + ((y & 1) ? (x ^ 4) : x)];
            }
        }
        load_multi_block(K0(a), 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 8, 0, 0, 0, 4, 3, 0, 0, true);
        slow0 = slow_binds();
        if (compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL)) {
            /* and it is the intended image */
            int bad = 0;

            for (y = 0; y < 8; y++) {
                for (x = 0; x < 16; x++) {
                    uint8_t ref[4], q[4], got[4];

                    decode_rgba16((img[(y * 16 + x) * 2] << 8) | img[(y * 16 + x) * 2 + 1], ref);
                    gx_quantize(GX_TF_RGB5A3, ref, q);
                    gx_texel(&gLoaded[0], x, y, got);
                    bad += memcmp(q, got, 4) != 0;
                }
            }
            CHECK(bad == 0, "%d texels differ from the unswapped image", bad);
        }
        CHECK(slow_binds() == slow0, "dxt 0 took the slow path");
    }
    test_end();

    test_begin("loadblock line of 6 words (dxt 342)");
    task_begin();
    a = ram_random(24 * 16 * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 24, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    slow0 = slow_binds();
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    CHECK(slow_binds() == slow0, "line of 6 words took the slow path");
    test_end();

    test_begin("loadblock with a wrong dxt (8b 12 wide)");
    task_begin();
    a = ram_random(12 * 8);
    load_texture_block(K0(a), G_IM_FMT_I, G_IM_SIZ_8b, 12, 8, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    slow0 = slow_binds();
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    CHECK(slow_binds() == slow0 + 1, "wrong dxt did not take the slow path");
    test_end();

    test_begin("loadblock line skipping (load tile line 2)");
    task_begin();
    a = ram_random(8 * 8 * 2);
    /* 8x8 RGBA16 = 2 words per row, dxt 1024: each row lands 2 extra words further, so rows are 4 words apart */
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, K0(a));
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 2, 0x20, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    dp_load_block(G_TX_LOADTILE, 0, 0, 63, 1024);
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 0x20, 0, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
    dp_set_tile_size(0, 0, 0, 7 << 2, 7 << 2);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("two textures: TEXEL0 + TEXEL1 (N64 logo)");
    task_begin();
    a = ram_random(32 * 32);
    b = ram_random(192 * 32);
    load_multi_block(K0(a), 0x100, 1, G_IM_FMT_I, G_IM_SIZ_8b, 32, 32, 0, 0, 0, 5, 5, 2, 11, false);
    load_texture_block(K0(b) + 0x180 * 3, G_IM_FMT_I, G_IM_SIZ_8b, 192, 2, 0, 0, 0, 0, 0, 0, 0);
    dp_set_tile_size(1, 37, (90 & 0x7F) - 3 * 4, 0, 0); /* degenerate size: the mask decides */
    slow0 = slow_binds();
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    compare_tile(1, GX_TEXMAP1, GX_TF_I8, NULL);
    CHECK(slow_binds() == slow0, "N64 logo textures took the slow path");
    CHECK(gLoaded[1].w == 32 && gLoaded[1].h == 32 && gLoaded[1].wrapS == GX_REPEAT, "shine %dx%d wrap %d",
          gLoaded[1].w, gLoaded[1].h, gLoaded[1].wrapS);
    compare_sampling(1, GX_TEXMAP1);
    test_end();

    test_begin("overlapping loads");
    task_begin();
    a = ram_random(64 * 32);
    b = ram_random(16 * 16);
    load_texture_block(K0(a), G_IM_FMT_I, G_IM_SIZ_8b, 64, 32, 0, 0, 0, 6, 5, 0, 0);
    load_multi_block(K0(b), 0x10, 1, G_IM_FMT_I, G_IM_SIZ_8b, 16, 16, 0, 0, 0, 4, 4, 0, 0, false);
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    compare_tile(1, GX_TEXMAP1, GX_TF_I8, NULL);
    test_end();

    test_begin("loadtile read from an odd row");
    task_begin();
    a = ram_random(32 * 16 * 2);
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 0, 0, 15, 7, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 4, 2, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
    dp_set_tile_size(2, 0, 0, 15 << 2, 6 << 2);
    compare_tile(2, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 8, 3, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0); /* even row: fast */
    dp_set_tile_size(3, 0, 0, 15 << 2, 5 << 2);
    compare_tile(3, GX_TEXMAP1, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("RGBA32 loadtile, odd row offset");
    task_begin();
    a = ram_random(16 * 16 * 4);
    load_multi_tile(K0(a), 0, 0, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 2, 3, 9, 10, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_32b, 2, 2, 1, 0, G_TX_CLAMP, 0, 0, G_TX_CLAMP, 0, 0);
    dp_set_tile_size(1, 0, 0, 7 << 2, 6 << 2);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGBA8, NULL);
    compare_tile(1, GX_TEXMAP1, GX_TF_RGBA8, NULL);
    test_end();

    test_begin("partial palette (64 of 256 entries)");
    task_begin();
    pal = ram_random(64 * 2);
    a = ram_random(32 * 16);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(64, 256, K0(pal));
    load_texture_block(K0(a), G_IM_FMT_CI, G_IM_SIZ_8b, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    slow0 = slow_binds();
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    CHECK(slow_binds() == slow0, "partial palette took the slow path");
    /* the same palette address with more entries is another texture */
    task_begin();
    b = ram_random(64 * 2);
    memcpy(gRam + pal + 64 * 2, gRam + b, 64 * 2);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(128, 256, K0(pal));
    load_texture_block(K0(a), G_IM_FMT_CI, G_IM_SIZ_8b, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    /* the rest of the palette from an older load: slow path, still exact */
    task_begin();
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(256, 256, K0(ram_random(256 * 2)));
    load_tlut(32, 256, K0(pal));
    load_texture_block(K0(a), G_IM_FMT_CI, G_IM_SIZ_8b, 32, 16, 0, 0, 0, 5, 4, 0, 0);
    slow0 = slow_binds();
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    CHECK(slow_binds() == slow0, "texels took the slow path");
    test_end();

    test_begin("CI4 palette 2 of a 256-entry TLUT");
    task_begin();
    pal = ram_random(256 * 2);
    a = ram_random(16 * 16 / 2);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(256, 256, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 2, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("CI texels wrap at 2 KB (TLUT half)");
    task_begin();
    a = ram_random(64 * 32);
    b = ram_random(16 * 8);
    pal = ram_random(16 * 2);
    load_texture_block(K0(a), G_IM_FMT_I, G_IM_SIZ_8b, 64, 32, 0, 0, 0, 6, 5, 0, 0);
    load_multi_block(K0(b), 0x120, 1, G_IM_FMT_CI, G_IM_SIZ_8b, 16, 8, 0, 0, 0, 4, 3, 0, 0, false);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(16, 256 + 15 * 16, K0(pal));
    load_multi_block_4b(K0(b), 0x120, 2, G_IM_FMT_CI, 16, 8, 15, 0, 0, 4, 3, 0, 0);
    compare_tile(1, GX_TEXMAP1, GX_TF_RGB5A3, NULL); /* reads the low half: the I8 texture */
    compare_tile(2, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    test_end();

    test_begin("mipmap tiles (palm tree DL)");
    task_begin();
    a = ram_random(16 * 16 * 2);
    b = ram_random(8 * 8 * 2);
    sp_othermode(G_SETOTHERMODE_H, G_MDSFT_TEXTLOD, 1, G_TL_LOD);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
    load_multi_block(K0(a), 0x0000, 1, G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, 0, 0, 4, 4, 0, 0, false);
    load_multi_block(K0(b), 0x0040, 2, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 0, 0, 0, 3, 3, 1, 1, false);
    load_multi_block(K0(b), 0x0040, 3, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 0, 0, 0, 3, 3, 1, 1, false);
    slow0 = slow_binds();
    for (i = 0; i < 4; i++) {
        compare_tile(i, i & 1, GX_TF_RGB5A3, NULL);
    }
    CHECK(slow_binds() == slow0, "mipmap tiles took the slow path");
    test_end();

    test_begin("more loads than records");
    task_begin();
    for (i = 0; i < 40; i++) {
        a = ram_random(8 * 8 * 2);
        load_multi_block(K0(a), (i % 30) * 0x10, i & 7, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 0, 0, 0, 3, 3, 0, 0, false);
    }
    for (i = 0; i < 8; i++) {
        compare_tile(i, i & 1, GX_TF_RGB5A3, NULL);
    }
    test_end();

    test_begin("degenerate clamped tile: size capped");
    task_begin();
    a = ram_random(16 * 16 * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    dp_set_tile_size(0, 100 << 2, 90 << 2, 10 << 2, 5 << 2); /* lower right < upper left: clamps at ~930 texels */
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    CHECK(gLoaded[0].w == 16 && gLoaded[0].h == 128, "capped to %dx%d", gLoaded[0].w, gLoaded[0].h);
    test_end();

    /* A newer load whose TMEM range covers an older one does not always write every byte of it */
    test_begin("loads with gaps keep older data");
    task_begin();
    a = ram_random(8 * 8);
    b = ram_random(64 * 2);
    load_texture_block(K0(a), G_IM_FMT_I, G_IM_SIZ_8b, 8, 8, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
    /* LOADTILE of 4 rows of one word every 4 words: TMEM [0, 104) with gaps over the I8 texture */
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, K0(b));
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    dp_load_tile(G_TX_LOADTILE, 0, 0, 3 << 2, 3 << 2);
    compare_tile(0, GX_TEXMAP0, GX_TF_I8, NULL);
    task_begin();
    a = ram_random(8);
    b = ram_random(12 * 4);
    load_multi_block(K0(a), 1, 1, G_IM_FMT_I, G_IM_SIZ_8b, 8, 1, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0, false);
    /* 32-bit LOADBLOCK, 3 RAM words per line (dxt 683): words 2 and 3 land in the same half of TMEM word 1 */
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_32b, 1, K0(b));
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_32b, 0, 0, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
    dp_load_block(G_TX_LOADTILE, 0, 0, 11, 683);
    compare_tile(1, GX_TEXMAP1, GX_TF_I8, NULL);
    test_end();

    test_begin("unresolved tile: bind fails");
    task_begin();
    dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 0, 0, 0, 0, 4, 0, 0, 4, 0);
    dp_set_tile_size(0, 0, 0, 15 << 2, 15 << 2);
    {
        GfxTexBinding bb;

        CHECK(!gfx_tex_bind(0, GX_TEXMAP0, &bb) && !bb.valid, "bind of an unloaded tile succeeded");
    }
    test_end();
}

/* ================================================================================================ */
/* Tests: wrap / mirror / clamp / mask / shift / offset                                              */
/* ================================================================================================ */

static void wrap_case(const char* name, int w, int h, int cms, int cmt, int masks, int maskt, int shifts, int shiftt,
                      int uls, int ult, int lrs, int lrt) {
    uint32_t a;
    int fullW = w, fullH = h;

    test_begin(name);
    task_begin();
    a = ram_random(fullW * fullH * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, fullW, fullH, 0, cms, cmt, masks, maskt, shifts, shiftt);
    dp_set_tile_size(0, uls, ult, lrs, lrt);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    compare_sampling(0, GX_TEXMAP0);
    test_end();
}

static void test_wrap(void) {
    wrap_case("wrap: repeat 16x16", 16, 16, 0, 0, 4, 4, 0, 0, 0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: mirror 16x16", 16, 16, G_TX_MIRROR, G_TX_MIRROR, 4, 4, 0, 0, 0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: clamp, no mask", 16, 8, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0, 0, 0, 15 << 2, 7 << 2);
    wrap_case("wrap: no mask means clamp", 16, 8, 0, G_TX_MIRROR, 0, 0, 0, 0, 0, 0, 15 << 2, 7 << 2);
    wrap_case("wrap: mask 8 in a clamp of 16", 16, 16, G_TX_CLAMP, G_TX_CLAMP, 3, 3, 0, 0, 0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: mirror mask 8, clamp 16", 16, 16, G_TX_CLAMP | G_TX_MIRROR, G_TX_CLAMP | G_TX_MIRROR, 3, 3, 0, 0,
              0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: mask 32 larger than clamp", 16, 16, G_TX_CLAMP, G_TX_CLAMP, 5, 5, 0, 0, 0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: mask beyond loaded line", 16, 16, 0, 0, 5, 4, 0, 0, 0, 0, 15 << 2, 15 << 2);
    wrap_case("wrap: clamp tile larger than data", 16, 8, G_TX_CLAMP, G_TX_CLAMP, 4, 3, 0, 0, 0, 0, 40 << 2, 20 << 2);
    wrap_case("shift: s >> 1, t >> 2", 16, 16, 0, 0, 4, 4, 1, 2, 0, 0, 15 << 2, 15 << 2);
    wrap_case("shift: s << 1 (15), t << 5 (11)", 16, 16, 0, G_TX_MIRROR, 4, 4, 15, 11, 0, 2, 15 << 2, 15 << 2);
    wrap_case("offset: uls/ult with fractions", 16, 16, 0, 0, 4, 4, 0, 0, (5 << 2) | 2, (3 << 2) | 1, 20 << 2, 18 << 2);
    wrap_case("offset: clamp with uls 4", 32, 16, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0, 4 << 2, 2 << 2, 27 << 2, 13 << 2);
    /* the N64 logo's shine: lower right 0,0; ult has half a texel so that t << 5 does not land on texel edges */
    wrap_case("degenerate tile size, masked", 32, 32, 0, 0, 5, 5, 2, 11, 37, 22, 0, 0);
}

/* ================================================================================================ */
/* Tests: rectangles and state commands                                                             */
/* ================================================================================================ */

static void texrect(int ulx, int uly, int lrx, int lry, int tile, int s, int t, int dsdx, int dtdy, bool flip) {
    uint32_t w0 = SHIFTL(flip ? G_TEXRECTFLIP : G_TEXRECT, 24, 8) | SHIFTL(lrx, 12, 12) | SHIFTL(lry, 0, 12);
    uint32_t w1 = SHIFTL(tile, 24, 3) | SHIFTL(ulx, 12, 12) | SHIFTL(uly, 0, 12);

    gfx_rdp_texrect(w0, w1, SHIFTL(s, 16, 16) | SHIFTL(t, 0, 16), SHIFTL(dsdx, 16, 16) | SHIFTL(dtdy, 0, 16), flip);
}

static void fillrect(int ulx, int uly, int lrx, int lry) {
    emit(SHIFTL(G_FILLRECT, 24, 8) | SHIFTL(lrx, 14, 10) | SHIFTL(lry, 2, 10),
         SHIFTL(ulx, 14, 10) | SHIFTL(uly, 2, 10));
}

#define FEQ(a, b) (fabsf((a) - (b)) < 1e-6f)

static void test_rects(void) {
    test_begin("texrect 1-cycle");
    task_begin();
    dp_set_cycle(G_CYC_1CYCLE);
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1 && FEQ(gTexRect.ulx, 97) && FEQ(gTexRect.uly, 94) && FEQ(gTexRect.lrx, 289) &&
              FEQ(gTexRect.lry, 96) && gTexRect.tile == 0 && FEQ(gTexRect.s, 0) && FEQ(gTexRect.t, 0) &&
              FEQ(gTexRect.dsdx, 1) && FEQ(gTexRect.dtdy, 1) && !gTexRect.flip,
          "1-cycle texrect: %g,%g %g,%g", gTexRect.ulx, gTexRect.uly, gTexRect.lrx, gTexRect.lry);
    texrect((10 << 2) | 1, (20 << 2) | 3, (30 << 2) | 2, 40 << 2, 5, -16, 3 << 5 | 8, -1024, 512, false);
    CHECK(FEQ(gTexRect.ulx, 10.25f) && FEQ(gTexRect.uly, 20.75f) && FEQ(gTexRect.lrx, 30.5f) && FEQ(gTexRect.lry, 40) &&
              gTexRect.tile == 5 && FEQ(gTexRect.s, -0.5f) && FEQ(gTexRect.t, 3.25f) && FEQ(gTexRect.dsdx, -1) &&
              FEQ(gTexRect.dtdy, 0.5f),
          "fractions: %g,%g %g,%g s %g t %g d %g %g", gTexRect.ulx, gTexRect.uly, gTexRect.lrx, gTexRect.lry,
          gTexRect.s, gTexRect.t, gTexRect.dsdx, gTexRect.dtdy);
    texrect(10 << 2, 10 << 2, 10 << 2, 20 << 2, 0, 0, 0, 1024, 1024, false);
    CHECK(gTexRect.calls == 2, "empty rectangle drawn");
    test_end();

    test_begin("texrect copy mode, flip");
    task_begin();
    dp_set_cycle(G_CYC_COPY);
    gTexRect.calls = 0;
    texrect(10 << 2, 20 << 2, 41 << 2, 51 << 2, 2, 3 << 5, 5 << 5, 4 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1 && FEQ(gTexRect.ulx, 10) && FEQ(gTexRect.uly, 20) && FEQ(gTexRect.lrx, 42) &&
              FEQ(gTexRect.lry, 52) && gTexRect.tile == 2 && FEQ(gTexRect.s, 3) && FEQ(gTexRect.t, 5) &&
              FEQ(gTexRect.dsdx, 1) && FEQ(gTexRect.dtdy, 1),
          "copy texrect: %g,%g %g,%g dsdx %g", gTexRect.ulx, gTexRect.uly, gTexRect.lrx, gTexRect.lry, gTexRect.dsdx);
    texrect(0, 0, 15 << 2, 31 << 2, 1, 0, 0, 4 << 10, 1 << 10, true);
    CHECK(gTexRect.flip && FEQ(gTexRect.lrx, 16) && FEQ(gTexRect.lry, 32), "flip");
    test_end();

    test_begin("fill rectangles");
    task_begin();
    gFillRect.calls = 0;
    dp_set_cycle(G_CYC_FILL);
    fillrect(0, 0, 319, 239);
    CHECK(gFillRect.calls == 1 && FEQ(gFillRect.ulx, 0) && FEQ(gFillRect.uly, 0) && FEQ(gFillRect.lrx, 320) &&
              FEQ(gFillRect.lry, 240),
          "FILL: %g,%g %g,%g", gFillRect.ulx, gFillRect.uly, gFillRect.lrx, gFillRect.lry);
    dp_set_cycle(G_CYC_1CYCLE);
    fillrect(0, 0, 319, 239);
    CHECK(gFillRect.calls == 2 && FEQ(gFillRect.lrx, 319) && FEQ(gFillRect.lry, 239), "1-cycle: %g,%g", gFillRect.lrx,
          gFillRect.lry);
    dp_set_cycle(G_CYC_2CYCLE);
    fillrect(5, 6, 5, 9);
    CHECK(gFillRect.calls == 2, "empty 2-cycle fill drawn");
    dp_set_cycle(G_CYC_COPY);
    fillrect(5, 6, 5, 9);
    CHECK(gFillRect.calls == 3 && FEQ(gFillRect.ulx, 5) && FEQ(gFillRect.lrx, 6) && FEQ(gFillRect.lry, 10),
          "COPY single column: %g %g", gFillRect.ulx, gFillRect.lrx);
    test_end();
}

static void test_state(void) {
    uint32_t h;
    int flushes;

    test_begin("other modes (F3DEX2 encoding)");
    task_begin();
    gGfxRdp.dirty = 0;
    dp_set_cycle(G_CYC_2CYCLE);
    CHECK((gGfxRdp.otherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE, "cycle type %08X", gGfxRdp.otherModeH);
    CHECK((gGfxRdp.dirty & (GFX_DIRTY_OTHERMODE | GFX_DIRTY_TEXTURES)) == (GFX_DIRTY_OTHERMODE | GFX_DIRTY_TEXTURES),
          "dirty %X", gGfxRdp.dirty);
    h = gGfxRdp.otherModeH;
    dp_set_filter(G_TF_POINT);
    CHECK(gGfxRdp.otherModeH == ((h & ~(3u << G_MDSFT_TEXTFILT)) | G_TF_POINT), "filter");
    dp_set_tlut(G_TT_IA16);
    CHECK((gGfxRdp.otherModeH & (3 << G_MDSFT_TEXTLUT)) == G_TT_IA16, "tlut");
    gGfxRdp.dirty = 0;
    sp_othermode(G_SETOTHERMODE_L, G_MDSFT_RENDERMODE, 29, 0x0C184240); /* G_RM_AA_ZB_OPA_SURF(2) */
    sp_othermode(G_SETOTHERMODE_L, G_MDSFT_ALPHACOMPARE, 2, G_AC_THRESHOLD);
    sp_othermode(G_SETOTHERMODE_L, G_MDSFT_ZSRCSEL, 1, G_ZS_PRIM);
    CHECK(gGfxRdp.otherModeL == (0x0C184240 | G_AC_THRESHOLD | G_ZS_PRIM), "L %08X", gGfxRdp.otherModeL);
    CHECK(gGfxRdp.dirty == GFX_DIRTY_OTHERMODE, "render mode only dirties OTHERMODE (%X)", gGfxRdp.dirty);
    sp_othermode(G_SETOTHERMODE_L, G_MDSFT_ALPHACOMPARE, 2, G_AC_NONE);
    CHECK(gGfxRdp.otherModeL == (0x0C184240 | G_ZS_PRIM), "L after clear %08X", gGfxRdp.otherModeL);
    emit(SHIFTL(G_RDPSETOTHERMODE, 24, 8) | 0x00102CF0, 0x00552078);
    CHECK(gGfxRdp.otherModeH == 0x00102CF0 && gGfxRdp.otherModeL == 0x00552078, "RDPSETOTHERMODE");
    test_end();

    test_begin("colors, combine, scissor, tiles");
    task_begin();
    gGfxRdp.dirty = 0;
    emit(SHIFTL(G_SETPRIMCOLOR, 24, 8) | SHIFTL(3, 8, 8) | SHIFTL(0x80, 0, 8), 0x11223344);
    CHECK(gGfxRdp.primColor == 0x11223344 && gGfxRdp.primLodMin == 3 && gGfxRdp.primLodFrac == 0x80 &&
              gGfxRdp.dirty == GFX_DIRTY_COLORS,
          "prim color");
    emit(SHIFTL(G_SETENVCOLOR, 24, 8), 0x55667788);
    emit(SHIFTL(G_SETFOGCOLOR, 24, 8), 0x99AABBCC);
    emit(SHIFTL(G_SETBLENDCOLOR, 24, 8), 0x01020304);
    emit(SHIFTL(G_SETFILLCOLOR, 24, 8), 0xFFFCFFFC);
    CHECK(gGfxRdp.envColor == 0x55667788 && gGfxRdp.fogColor == 0x99AABBCC && gGfxRdp.blendColor == 0x01020304 &&
              gGfxRdp.fillColor == 0xFFFCFFFC,
          "colors");
    emit(SHIFTL(G_SETPRIMDEPTH, 24, 8), 0x7FFF0001);
    CHECK(gGfxRdp.primDepthZ == 0x7FFF && gGfxRdp.primDepthDZ == 1, "prim depth");
    gGfxRdp.dirty = 0;
    emit(SHIFTL(G_SETCOMBINE, 24, 8) | 0x00FC1234, 0x5678ABCD);
    CHECK(gGfxRdp.combineHi == 0x00FC1234 && gGfxRdp.combineLo == 0x5678ABCD && gGfxRdp.dirty == GFX_DIRTY_COMBINE,
          "combine");
    gGfxRdp.dirty = 0;
    emit(SHIFTL(G_SETCOMBINE, 24, 8) | 0x00FC1234, 0x5678ABCD);
    CHECK(gGfxRdp.dirty == 0, "same combine dirtied state");
    emit(SHIFTL(G_SETSCISSOR, 24, 8) | SHIFTL(4, 12, 12) | SHIFTL(8, 0, 12),
         SHIFTL(G_SC_NON_INTERLACE, 24, 2) | SHIFTL(1280, 12, 12) | SHIFTL(960, 0, 12));
    CHECK(gGfxRdp.scissorUlx == 4 && gGfxRdp.scissorUly == 8 && gGfxRdp.scissorLrx == 1280 &&
              gGfxRdp.scissorLry == 960 && (gGfxRdp.dirty & GFX_DIRTY_SCISSOR),
          "scissor");
    dp_set_tile(G_IM_FMT_CI, G_IM_SIZ_4b, 0x1AB, 0x155, 6, 0xC, 3, 0xA, 0x5, 2, 0x7, 0xB);
    {
        const GfxTile* t = &gGfxRdp.tiles[6];

        CHECK(t->fmt == G_IM_FMT_CI && t->siz == G_IM_SIZ_4b && t->line == 0x1AB && t->tmem == 0x155 &&
                  t->palette == 0xC && t->cmt == 3 && t->maskt == 0xA && t->shiftt == 5 && t->cms == 2 &&
                  t->masks == 7 && t->shifts == 0xB,
              "settile fields");
    }
    dp_set_tile_size(6, 0x123, 0x456, 0x789, 0xABC);
    CHECK(gGfxRdp.tiles[6].uls == 0x123 && gGfxRdp.tiles[6].ult == 0x456 && gGfxRdp.tiles[6].lrs == 0x789 &&
              gGfxRdp.tiles[6].lrt == 0xABC && (gGfxRdp.dirty & GFX_DIRTY_TEXTURES),
          "settilesize");
    test_end();

    test_begin("images and segments");
    task_begin();
    gGfxSegments[6] = 0x00123400;
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 0x06000010);
    CHECK(gGfxRdp.texImageAddr == 0x80123410 && gGfxRdp.texImageWidth == 64 && gGfxRdp.texImageSiz == G_IM_SIZ_16b,
          "settimg segmented: %08X", gGfxRdp.texImageAddr);
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 1, 0xA0200000);
    CHECK(gGfxRdp.texImageAddr == 0x80200000, "settimg KSEG1: %08X", gGfxRdp.texImageAddr);
    flushes = gGxFlushes;
    emit(SHIFTL(G_SETCIMG, 24, 8) | SHIFTL(G_IM_FMT_RGBA, 21, 3) | SHIFTL(G_IM_SIZ_16b, 19, 2) | 319, 0x0F000000);
    CHECK(gGxFlushes == flushes + 1 && gGfxRdp.colorImageAddr == 0x0F000000 && gGfxRdp.colorImageWidth == 320 &&
              gGfxRdp.colorImageSiz == G_IM_SIZ_16b,
          "setcimg");
    emit(SHIFTL(G_SETCIMG, 24, 8) | SHIFTL(G_IM_FMT_RGBA, 21, 3) | SHIFTL(G_IM_SIZ_16b, 19, 2) | 319, 0x0F000000);
    CHECK(gGxFlushes == flushes + 1, "same color image flushed");
    emit(SHIFTL(G_SETCIMG, 24, 8) | SHIFTL(G_IM_FMT_RGBA, 21, 3) | SHIFTL(G_IM_SIZ_16b, 19, 2) | 319, 0x80300000);
    CHECK(gGxFlushes == flushes + 2, "color image change not flushed");
    emit(SHIFTL(G_SETZIMG, 24, 8), 0x80300000);
    CHECK(gGfxRdp.zImageAddr == 0x80300000, "setzimg");
    /* gbi.h packs 12 bits of width, the RDP reads 10 */
    dp_set_timg(G_IM_FMT_RGBA, G_IM_SIZ_16b, 0x501, 0x80200000);
    CHECK(gGfxRdp.texImageWidth == 0x101, "settimg width %u", gGfxRdp.texImageWidth);
    emit(SHIFTL(G_SETCIMG, 24, 8) | SHIFTL(G_IM_FMT_RGBA, 21, 3) | SHIFTL(G_IM_SIZ_16b, 19, 2) | 0x53F, 0x80300000);
    CHECK(gGfxRdp.colorImageWidth == 0x140, "setcimg width %u", gGfxRdp.colorImageWidth);
    test_end();

    test_begin("syncs, convert, key, unknown commands");
    task_begin();
    {
        int logs = gLogs;
        GfxRdpState before = gGfxRdp;

        dp_sync(G_RDPFULLSYNC);
        dp_sync(G_RDPPIPESYNC);
        dp_sync(G_RDPTILESYNC);
        dp_sync(G_RDPLOADSYNC);
        emit(SHIFTL(G_SETCONVERT, 24, 8) | SHIFTL(175, 13, 9) | SHIFTL(-43, 4, 9) | ((uint32_t)(-89 & 0x1FF) >> 5),
             SHIFTL(-89, 27, 5) | SHIFTL(222, 18, 9) | SHIFTL(114, 9, 9) | SHIFTL(42, 0, 9));
        emit(SHIFTL(G_SETKEYR, 24, 8), 0x01000080);
        emit(SHIFTL(G_SETKEYGB, 24, 8) | 0x123456, 0x80108020);
        CHECK(memcmp(&before, &gGfxRdp, sizeof(before)) == 0 && gLogs == logs, "state changed or logged");
        emit(SHIFTL(0xC8, 24, 8), 0); /* RDP triangle: never sent by F3DZEX2 */
        emit(SHIFTL(0xC8, 24, 8), 0);
        CHECK(gLogs == logs + 1, "unknown command logged %d times", gLogs - logs);
    }
    test_end();
}

/* ================================================================================================ */
/* Tests: cache                                                                                     */
/* ================================================================================================ */

static void test_cache_basic(void) {
    GfxTexStats s0, s1;
    GfxTexBinding b;
    uint32_t a, pal;

    test_begin("cache: hit, content change, palette change");
    task_begin();
    a = ram_random(32 * 32 * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    gfx_tex_get_stats(&s0);
    gfx_tex_bind(0, GX_TEXMAP0, &b); /* repeated bind: memo */
    gfx_tex_get_stats(&s1);
    CHECK(s1.hits == s0.hits + 1 && s1.misses == s0.misses, "repeated bind: hits %u->%u misses %u->%u", s0.hits,
          s1.hits, s0.misses, s1.misses);

    /* next task: same texture, unchanged */
    task_begin();
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    gfx_tex_get_stats(&s0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    gfx_tex_get_stats(&s1);
    CHECK(s1.hits == s0.hits + 1 && s1.misses == s0.misses && s1.reconverts == s0.reconverts, "next task: no hit");

    /* the game rewrites the texture: same task -> still the old one (checked once per task) */
    gRam[a + 100] ^= 0xFF;
    gRam[a + 101] ^= 0xFF;
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    gfx_tex_get_stats(&s0);
    gfx_tex_bind(0, GX_TEXMAP0, &b);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts, "converted again within the task");
    /* next task: converted again */
    task_begin();
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
    gfx_tex_get_stats(&s0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts + 1 && s1.misses == s0.misses, "rewrite not noticed");

    /* palette change */
    task_begin();
    pal = ram_random(16 * 2);
    a = ram_random(16 * 16 / 2);
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(16, 256 + 16, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 1, 0, 0, 4, 4, 0, 0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    gRam[pal + 6] ^= 0x5A;
    task_begin();
    dp_set_tlut(G_TT_RGBA16);
    load_tlut(16, 256 + 16, K0(pal));
    load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 1, 0, 0, 4, 4, 0, 0);
    gfx_tex_get_stats(&s0);
    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts + 1, "palette change not noticed");

    /* filter: bilinear moves the offset by half a texel, copy mode samples points */
    task_begin();
    a = ram_random(16 * 16 * 2);
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
    dp_set_tile_size(0, 8, 4, (15 << 2) + 8, (15 << 2) + 4);
    dp_set_filter(G_TF_BILERP);
    gfx_tex_bind(0, GX_TEXMAP0, &b);
    CHECK(b.linear && FEQ(b.sOffset, 2.0f - 0.5f) && FEQ(b.tOffset, 1.0f - 0.5f) && gLoaded[0].minf == GX_LINEAR,
          "bilinear binding: offset %g %g", b.sOffset, b.tOffset);
    dp_set_cycle(G_CYC_COPY);
    gfx_tex_bind(0, GX_TEXMAP0, &b);
    CHECK(!b.linear && FEQ(b.sOffset, 2.0f) && gLoaded[0].magf == GX_NEAR, "copy mode binding");
    test_end();
}

/* Many textures of different sizes (expanded masked regions inside large clamps) through a small cache */
static void test_cache_pressure(void) {
    GfxTexStats s0, s1;
    uint32_t srcs[48];
    int sizes[48];
    int i, round;

    test_begin("cache: LRU eviction under pressure");
    for (i = 0; i < 48; i++) {
        srcs[i] = ram_random(16 * 16 * 2);
        sizes[i] = 8 + (int)(rnd() % 160);
    }
    gfx_tex_get_stats(&s0);
    for (round = 0; round < 4; round++) {
        task_begin();
        for (i = 0; i < 48; i++) {
            int k = (round & 1) ? 47 - i : i;

            load_texture_block(K0(srcs[k]), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP | (k & 1), G_TX_CLAMP, 4,
                               4, 0, 0);
            dp_set_tile_size(0, 0, 0, (sizes[k] - 1) << 2, ((sizes[k] / 2 + 4) - 1) << 2);
            compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        }
    }
    gfx_tex_get_stats(&s1);
    CHECK(s1.evictions > s0.evictions, "no evictions (%u)", s1.evictions - s0.evictions);
    CHECK(s1.syncs > s0.syncs, "no GX sync when evicting textures of the same task");
    CHECK(s1.bytesUsed <= s1.bytesTotal, "bytes used %u of %u", s1.bytesUsed, s1.bytesTotal);
    CHECK(s1.failures == s0.failures, "%u binds failed", s1.failures - s0.failures);
    printf("    %u binds, %u hits, %u misses, %u evictions, %u syncs, %u entries, %u/%u KB\n", s1.binds - s0.binds,
           s1.hits - s0.hits, s1.misses - s0.misses, s1.evictions - s0.evictions, s1.syncs - s0.syncs, s1.entries,
           s1.bytesUsed >> 10, s1.bytesTotal >> 10);

    /* textures already converted come back from the cache with their contents intact */
    task_begin();
    for (i = 44; i < 48; i++) {
        load_texture_block(K0(srcs[i]), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP | (i & 1), G_TX_CLAMP, 4, 4,
                           0, 0);
        dp_set_tile_size(0, 0, 0, (sizes[i] - 1) << 2, ((sizes[i] / 2 + 4) - 1) << 2);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    }
    test_end();
}

/* Binding TEXEL1 must not free the texture just loaded for TEXEL0 of the same draw: with the 256 KB test cache, a
 * 128 KB TEXEL0 and a TEXEL1 that needs the whole cache, the TEXEL1 bind fails instead of overwriting TEXEL0. A
 * repeated (memoized) TEXEL0 bind also counts as recent use. */
static void test_cache_pinning(void) {
    GfxTexStats s0, s1;
    GfxTexBinding b;
    uint32_t a, c;
    uint8_t* snapshot;
    int i;

    test_begin("cache: TEXEL0 kept while binding TEXEL1");
    task_begin();
    a = ram_random(16 * 16 * 2);
    c = ram_random(16 * 16 * 4);
    /* tile 0: 256x256 RGB5A3 (128 KB), a 16x16 masked region expanded into a 256x256 clamp */
    load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 4, 4, 0, 0);
    dp_set_tile_size(0, 0, 0, 255 << 2, 255 << 2);
    /* tile 1: 256x256 RGBA8 (256 KB) */
    load_multi_block(K0(c), 0x80, 1, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 4, 4, 0, 0, false);
    dp_set_tile_size(1, 0, 0, 255 << 2, 255 << 2);

    compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
    snapshot = malloc(128 * 1024);
    memcpy(snapshot, gLoaded[0].img, 128 * 1024);
    for (i = 0; i < 2; i++) {
        if (i == 1) {
            CHECK(gfx_tex_bind(0, GX_TEXMAP0, &b), "memoized TEXEL0 bind failed");
        }
        gfx_tex_get_stats(&s0);
        CHECK(!gfx_tex_bind(1, GX_TEXMAP1, &b), "TEXEL1 bound: it can only fit by freeing TEXEL0");
        gfx_tex_get_stats(&s1);
        CHECK(s1.failures == s0.failures + 1, "TEXEL1 bind did not fail");
        CHECK(memcmp(snapshot, gLoaded[0].img, 128 * 1024) == 0, "TEXEL0 texture memory overwritten");
    }
    free(snapshot);
    /* next task: TEXEL0 is no longer pinned, TEXEL1 fits */
    task_begin();
    load_multi_block(K0(c), 0x80, 1, G_IM_FMT_RGBA, G_IM_SIZ_32b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP, 4, 4, 0, 0, false);
    dp_set_tile_size(1, 0, 0, 255 << 2, 255 << 2);
    compare_tile(1, GX_TEXMAP1, GX_TF_RGBA8, NULL);
    test_end();
}

/* ================================================================================================ */
/* Tests: texture rectangles in one-pixel strips, COPY mode wrapping                                */
/* ================================================================================================ */

static bool rect_call_is(int i, float ulx, float uly, float lrx, float lry, float s, float t, float dsdx, float dtdy,
                         bool flip) {
    const TexRectCall* c = &gTexRectLog[i];

    return FEQ(c->ulx, ulx) && FEQ(c->uly, uly) && FEQ(c->lrx, lrx) && FEQ(c->lry, lry) && FEQ(c->s, s) &&
           FEQ(c->t, t) && FEQ(c->dsdx, dsdx) && FEQ(c->dtdy, dtdy) && c->flip == flip && c->tile == 0;
}

static void test_rect_strips(void) {
    int i;

    test_begin("texrect strips: N64 logo shine");
    task_begin();
    dp_set_cycle(G_CYC_2CYCLE);
    /* ConsoleLogo_Draw: text in tile 0 (no mask), shine in tile 1 (mask 5, t << 5, s >> 2) */
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, 0, 5, 11, 0, 5, 2);
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 2, "%d rectangles", gTexRect.calls);
    CHECK(rect_call_is(0, 97, 94, 289, 95, 0, 0, 1, 0, false) && rect_call_is(1, 97, 95, 289, 96, 0, 1, 1, 0, false),
          "rows: t %g %g, dtdy %g", gTexRectLog[0].t, gTexRectLog[1].t, gTexRectLog[0].dtdy);
    /* 1-cycle mode reads tile 0 only */
    dp_set_cycle(G_CYC_1CYCLE);
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1 && rect_call_is(0, 97, 94, 289, 96, 0, 0, 1, 1, false), "1-cycle: %d rectangles",
          gTexRect.calls);
    /* a clamped shine, or one that repeats slower than every two pixels, is drawn whole */
    dp_set_cycle(G_CYC_2CYCLE);
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, G_TX_CLAMP, 5, 11, 0, 5, 2);
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1, "clamped: %d rectangles", gTexRect.calls);
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, 0, 5, 12, 0, 5, 2); /* 16 per row: half the period */
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 2, "t << 4: %d rectangles", gTexRect.calls);
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, 0, 5, 13, 0, 5, 2); /* 8 per row */
    gTexRect.calls = 0;
    texrect(97 << 2, 94 << 2, (97 + 192) << 2, 96 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1, "t << 3: %d rectangles", gTexRect.calls);
    /* columns, and fractional rectangle edges */
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, 0, 0, 0, G_TX_MIRROR, 4, 11);
    gTexRect.calls = 0;
    texrect((10 << 2) | 2, 20 << 2, 13 << 2, 22 << 2, 0, 3 << 5, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 3 && rect_call_is(0, 10.5f, 20, 11, 22, 3, 0, 0, 1, false) &&
              rect_call_is(1, 11, 20, 12, 22, 3.5f, 0, 0, 1, false) &&
              rect_call_is(2, 12, 20, 13, 22, 4.5f, 0, 0, 1, false),
          "columns: %d rectangles, s %g %g %g", gTexRect.calls, gTexRectLog[0].s, gTexRectLog[1].s, gTexRectLog[2].s);
    /* G_TEXRECTFLIP: s runs down the rectangle, so a fast s splits rows */
    gTexRect.calls = 0;
    texrect(10 << 2, 20 << 2, 14 << 2, 23 << 2, 0, 1 << 5, 2 << 5, 1 << 10, 1 << 10, true);
    CHECK(gTexRect.calls == 3, "flip: %d rectangles", gTexRect.calls);
    for (i = 0; i < 3 && gTexRect.calls == 3; i++) {
        CHECK(rect_call_is(i, 10, 20 + i, 14, 21 + i, 1 + i, 2, 0, 1, true), "flip row %d: s %g dsdx %g dtdy %g", i,
              gTexRectLog[i].s, gTexRectLog[i].dsdx, gTexRectLog[i].dtdy);
    }
    /* both axes */
    dp_set_tile(G_IM_FMT_I, G_IM_SIZ_8b, 4, 0x100, 1, 0, 0, 5, 11, 0, 5, 11);
    gTexRect.calls = 0;
    texrect(0, 0, 3 << 2, 2 << 2, 0, 0, 0, 1 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 6 && rect_call_is(4, 1, 1, 2, 2, 1, 1, 0, 0, false), "cells: %d rectangles",
          gTexRect.calls);
    /* COPY mode is never split */
    dp_set_cycle(G_CYC_COPY);
    gTexRect.calls = 0;
    texrect(0, 0, 3 << 2, 2 << 2, 0, 0, 0, 4 << 10, 1 << 10, false);
    CHECK(gTexRect.calls == 1, "COPY: %d rectangles", gTexRect.calls);
    test_end();

    test_begin("COPY mode masks without clamping");
    {
        uint32_t a = ram_random(16 * 16 * 2);

        task_begin();
        load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, G_TX_CLAMP, G_TX_CLAMP | G_TX_MIRROR, 3, 3,
                           0, 0);
        dp_set_tile_size(0, 0, 0, 31 << 2, 31 << 2);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        CHECK(gLoaded[0].w == 32 && gLoaded[0].wrapS == GX_CLAMP, "1-cycle: %dx%d wrap %d", gLoaded[0].w,
              gLoaded[0].h, gLoaded[0].wrapS);
        dp_set_cycle(G_CYC_COPY);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        CHECK(gLoaded[0].w == 8 && gLoaded[0].h == 8 && gLoaded[0].wrapS == GX_REPEAT && gLoaded[0].wrapT == GX_MIRROR,
              "COPY: %dx%d wrap %d %d", gLoaded[0].w, gLoaded[0].h, gLoaded[0].wrapS, gLoaded[0].wrapT);
        compare_sampling(0, GX_TEXMAP0);
        /* no mask: the tile size still bounds it */
        dp_set_tile(G_IM_FMT_RGBA, G_IM_SIZ_16b, 4, 0, 0, 0, G_TX_CLAMP, G_TX_CLAMP, 0, 0, 0, 0);
        dp_set_tile_size(0, 0, 0, 15 << 2, 15 << 2);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        CHECK(gLoaded[0].w == 16 && gLoaded[0].wrapS == GX_CLAMP, "COPY without mask");
    }
    test_end();
}

/* ================================================================================================ */
/* Tests: images bound straight from RAM                                                            */
/* ================================================================================================ */

/* Texel (x, y) of an image in RAM, decoded as the RDP does, as RGBA8 */
static void ref_image_texel(const uint8_t* img, int fmt, int siz, int stride, int x, int y, const uint8_t* pal,
                            bool palIA, uint8_t o[4]) {
    const uint8_t* row;
    int v;

    switch (siz) {
        case G_IM_SIZ_4b:
            row = img + y * ((stride + 1) / 2);
            v = (x & 1) ? (row[x / 2] & 0xF) : (row[x / 2] >> 4);
            break;
        case G_IM_SIZ_8b:
            row = img + y * stride;
            v = row[x];
            break;
        case G_IM_SIZ_16b:
            row = img + y * stride * 2;
            if (fmt == G_IM_FMT_YUV) {
                const uint8_t* p = row + (x & ~1) * 2;
                double yy = p[(x & 1) ? 3 : 1], u = p[0] - 128.0, vv = p[2] - 128.0;
                double c[3] = { yy + floor(175.0 * vv / 128.0 + 0.5), yy + floor((-43.0 * u - 89.0 * vv) / 128.0 + 0.5),
                                yy + floor(222.0 * u / 128.0 + 0.5) };
                int k;

                for (k = 0; k < 3; k++) {
                    o[k] = c[k] < 0 ? 0 : c[k] > 255 ? 255 : (uint8_t)c[k];
                }
                o[3] = 255;
                return;
            }
            v = (row[x * 2] << 8) | row[x * 2 + 1];
            if (fmt == G_IM_FMT_RGBA) {
                decode_rgba16(v, o);
            } else {
                decode_ia16(v, o);
            }
            return;
        default:
            row = img + y * stride * 4;
            memcpy(o, row + x * 4, 4);
            return;
    }
    if (pal != NULL) {
        uint16_t c = (pal[v * 2] << 8) | pal[v * 2 + 1];

        if (palIA) {
            decode_ia16(c, o);
        } else {
            decode_rgba16(c, o);
        }
    } else if (siz == G_IM_SIZ_4b && fmt == G_IM_FMT_IA) {
        int i = v >> 1;

        o[0] = o[1] = o[2] = (i << 5) | (i << 2) | (i >> 1);
        o[3] = (v & 1) ? 255 : 0;
    } else if (siz == G_IM_SIZ_4b && fmt != G_IM_FMT_CI) {
        o[0] = o[1] = o[2] = o[3] = v * 17;
    } else if (siz == G_IM_SIZ_8b && fmt == G_IM_FMT_IA) {
        o[0] = o[1] = o[2] = (v >> 4) * 17;
        o[3] = (v & 0xF) * 17;
    } else {
        o[0] = o[1] = o[2] = o[3] = v; /* I8, and CI without a palette (palette 0) */
    }
}

static bool compare_image(const char* what, const uint8_t* img, int fmt, int siz, int w, int h, int stride,
                          const uint8_t* pal, bool palIA, bool linear, int texMap, int expectFmt) {
    GfxTexBinding b;
    const StubTex* st;
    float u, v;
    int x, y, errors = 0;

    memset(&b, 0xA5, sizeof(b));
    if (!gfx_tex_bind_image(img, fmt, siz, w, h, stride, pal, palIA, linear, texMap, &b) || !b.valid) {
        CHECK(false, "%s: bind failed", what);
        return false;
    }
    st = &gLoaded[texMap];
    CHECK(st->w == w && st->h == h && st->fmt == expectFmt && st->wrapS == GX_CLAMP && st->wrapT == GX_CLAMP &&
              st->magf == (linear ? GX_LINEAR : GX_NEAR) && st->minf == st->magf,
          "%s: GX %dx%d fmt %d wrap %d %d filter %d", what, st->w, st->h, st->fmt, st->wrapS, st->wrapT, st->magf);
    CHECK(b.width == w && b.height == h && b.linear == linear && b.sOffset == 0.0f && b.tOffset == 0.0f &&
              b.sShiftScale == 1.0f && b.tShiftScale == 1.0f,
          "%s: binding %ux%u offsets %g %g scales %g %g", what, b.width, b.height, b.sOffset, b.tOffset, b.sShiftScale,
          b.tShiftScale);
    gfx_tex_uv(&b, 3.0f, 5.0f, &u, &v);
    CHECK(FEQ(u, 3.0f / w) && FEQ(v, 5.0f / h), "%s: uv %g %g", what, u, v);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t ref[4], q[4], got[4];

            ref_image_texel(img, fmt, siz, stride, x, y, pal, palIA, ref);
            gx_quantize(st->fmt, ref, q);
            gx_texel(st, x, y, got);
            if (memcmp(q, got, 4) != 0 && errors++ < 4) {
                CHECK(false, "%s texel (%d,%d): got %02X%02X%02X%02X expected %02X%02X%02X%02X", what, x, y, got[0],
                      got[1], got[2], got[3], q[0], q[1], q[2], q[3]);
            }
        }
    }
    CHECK(errors == 0, "%s: %d texels differ", what, errors);
    return errors == 0;
}

static uint8_t* ram_image(uint32_t bytes) {
    return gRam + ram_random(bytes);
}

static void test_images(void) {
    GfxTexStats s0, s1;
    GfxTexBinding b;
    uint8_t *img, *pal;
    int drawDones;

    test_begin("images: every format");
    task_begin();
    compare_image("RGBA16", ram_image(64 * 32 * 2), G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 32, 64, NULL, false, false,
                  GX_TEXMAP0, GX_TF_RGB5A3);
    compare_image("RGBA16 37x19, stride 40", ram_image(40 * 19 * 2), G_IM_FMT_RGBA, G_IM_SIZ_16b, 37, 19, 40, NULL,
                  false, true, GX_TEXMAP1, GX_TF_RGB5A3);
    compare_image("RGBA32", ram_image(24 * 10 * 4), G_IM_FMT_RGBA, G_IM_SIZ_32b, 20, 10, 24, NULL, false, false,
                  GX_TEXMAP0, GX_TF_RGBA8);
    compare_image("I4 33x17", ram_image(34 * 17 / 2), G_IM_FMT_I, G_IM_SIZ_4b, 33, 17, 34, NULL, false, false,
                  GX_TEXMAP0, GX_TF_I4);
    compare_image("I8", ram_image(30 * 9), G_IM_FMT_I, G_IM_SIZ_8b, 30, 9, 30, NULL, false, false, GX_TEXMAP0,
                  GX_TF_I8);
    compare_image("IA4", ram_image(16 * 8 / 2), G_IM_FMT_IA, G_IM_SIZ_4b, 16, 8, 16, NULL, false, false, GX_TEXMAP0,
                  GX_TF_IA4);
    compare_image("IA8", ram_image(17 * 5), G_IM_FMT_IA, G_IM_SIZ_8b, 17, 5, 17, NULL, false, false, GX_TEXMAP0,
                  GX_TF_IA4);
    compare_image("IA16", ram_image(12 * 12 * 2), G_IM_FMT_IA, G_IM_SIZ_16b, 12, 12, 12, NULL, false, false,
                  GX_TEXMAP0, GX_TF_IA8);
    pal = ram_image(16 * 2);
    compare_image("CI4 + RGBA16 TLUT", ram_image(16 * 16 / 2), G_IM_FMT_CI, G_IM_SIZ_4b, 16, 16, 16, pal, false, false,
                  GX_TEXMAP0, GX_TF_RGB5A3);
    pal = ram_image(256 * 2);
    compare_image("CI8 + IA16 TLUT", ram_image(40 * 20), G_IM_FMT_CI, G_IM_SIZ_8b, 40, 20, 40, pal, true, false,
                  GX_TEXMAP0, GX_TF_IA8);
    compare_image("CI8 without TLUT", ram_image(8 * 8), G_IM_FMT_CI, G_IM_SIZ_8b, 8, 8, 8, NULL, false, false,
                  GX_TEXMAP0, GX_TF_I8);
    compare_image("CI4 without TLUT", ram_image(8 * 8 / 2), G_IM_FMT_CI, G_IM_SIZ_4b, 8, 8, 8, NULL, false, false,
                  GX_TEXMAP0, GX_TF_I8);
    compare_image("YUV16", ram_image(16 * 8 * 2), G_IM_FMT_YUV, G_IM_SIZ_16b, 16, 8, 16, NULL, false, false,
                  GX_TEXMAP0, GX_TF_RGBA8);
    compare_image("YUV16 odd width", ram_image(16 * 8 * 2), G_IM_FMT_YUV, G_IM_SIZ_16b, 15, 8, 16, NULL, false, false,
                  GX_TEXMAP0, GX_TF_RGBA8);
    test_end();

    test_begin("images: 320x240 backgrounds");
    task_begin();
    /* Grandma's story: CI8 320x240 with a 256-entry RGBA16 TLUT; a framebuffer: RGBA16 320x240 */
    pal = ram_image(256 * 2);
    compare_image("CI8 320x240", ram_image(320 * 240), G_IM_FMT_CI, G_IM_SIZ_8b, 320, 240, 320, pal, false, true,
                  GX_TEXMAP0, GX_TF_RGB5A3);
    task_begin();
    compare_image("RGBA16 320x240", ram_image(320 * 240 * 2), G_IM_FMT_RGBA, G_IM_SIZ_16b, 320, 240, 320, NULL, false,
                  false, GX_TEXMAP0, GX_TF_RGB5A3);
    test_end();

    test_begin("images: limits");
    task_begin();
    img = ram_image(64 * 64 * 2);
    gfx_tex_get_stats(&s0);
    CHECK(!gfx_tex_bind_image(img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1025, 1, 1025, NULL, false, false, GX_TEXMAP0, &b) &&
              !b.valid,
          "1025 texels wide bound");
    CHECK(!gfx_tex_bind_image(img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 64, 64, 32, NULL, false, false, GX_TEXMAP0, &b),
          "stride below the width bound");
    CHECK(!gfx_tex_bind_image(NULL, G_IM_FMT_RGBA, G_IM_SIZ_16b, 8, 8, 8, NULL, false, false, GX_TEXMAP0, &b),
          "NULL image bound");
    CHECK(!gfx_tex_bind_image(img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 0, 8, 8, NULL, false, false, GX_TEXMAP0, &b),
          "empty image bound");
    /* 1024x256 RGBA16 is 512 KB: more than the 256 KB test cache */
    CHECK(!gfx_tex_bind_image(gRam + 0x200000, G_IM_FMT_RGBA, G_IM_SIZ_16b, 1024, 256, 1024, NULL, false, false,
                              GX_TEXMAP0, &b),
          "image larger than the cache bound");
    gfx_tex_get_stats(&s1);
    CHECK(s1.failures == s0.failures + 5, "%u failures", s1.failures - s0.failures);
    test_end();

    test_begin("images: cache and tile memo");
    task_begin();
    img = ram_image(32 * 32 * 2);
    compare_image("first", img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, false, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    gfx_tex_get_stats(&s0);
    compare_image("again", img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, false, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    gfx_tex_get_stats(&s1);
    CHECK(s1.hits == s0.hits + 1 && s1.misses == s0.misses, "second bind not a hit");
    /* the same pixels with another filter share the texture */
    compare_image("bilinear", img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, true, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    /* rewritten during the task: noticed only when the renderer says so */
    img[7] ^= 0x42;
    gfx_tex_get_stats(&s0);
    gfx_tex_bind_image(img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, false, GX_TEXMAP0, &b);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts, "converted again without gfx_tex_ram_written");
    gfx_tex_ram_written((uint32_t)(uintptr_t)(img + 6), 2);
    drawDones = gDrawDones;
    gfx_tex_get_stats(&s0);
    compare_image("rewritten", img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, false, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts + 1 && s1.syncs == s0.syncs + 1 && gDrawDones == drawDones + 1,
          "ram written: %u reconverts, %u syncs", s1.reconverts - s0.reconverts, s1.syncs - s0.syncs);
    /* next task: checked again, unchanged */
    task_begin();
    gfx_tex_get_stats(&s0);
    compare_image("next task", img, G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 32, NULL, false, false, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    gfx_tex_get_stats(&s1);
    CHECK(s1.hits == s0.hits + 1 && s1.reconverts == s0.reconverts, "next task not a hit");
    /* a palette change */
    pal = ram_image(16 * 2);
    img = ram_image(8 * 8 / 2);
    compare_image("CI4", img, G_IM_FMT_CI, G_IM_SIZ_4b, 8, 8, 8, pal, false, false, GX_TEXMAP0, GX_TF_RGB5A3);
    pal[3] ^= 0x81;
    task_begin();
    gfx_tex_get_stats(&s0);
    compare_image("CI4, palette changed", img, G_IM_FMT_CI, G_IM_SIZ_4b, 8, 8, 8, pal, false, false, GX_TEXMAP0,
                  GX_TF_RGB5A3);
    gfx_tex_get_stats(&s1);
    CHECK(s1.reconverts == s0.reconverts + 1, "palette change not noticed");
    /* a tile bound to the same map before and after an image */
    {
        uint32_t a = ram_random(16 * 16 * 2);
        void* tileImg;

        load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 16, 16, 0, 0, 0, 4, 4, 0, 0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        tileImg = gLoaded[0].img;
        compare_image("between tiles", img, G_IM_FMT_CI, G_IM_SIZ_4b, 8, 8, 8, pal, false, false, GX_TEXMAP0,
                      GX_TF_RGB5A3);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        CHECK(gLoaded[0].img == tileImg, "tile texture not reloaded after the image");
    }
    test_end();

    test_begin("cache: RAM rewritten during a task");
    {
        uint32_t a = ram_random(32 * 32 * 2), palA;
        int logs;

        task_begin();
        load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        /* elsewhere: nothing to do */
        gfx_tex_ram_written(K0(a) + 32 * 32 * 2 + 64, 64);
        gfx_tex_get_stats(&s0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        gfx_tex_get_stats(&s1);
        CHECK(s1.reconverts == s0.reconverts && s1.hits == s0.hits + 1, "unrelated range: %u reconverts",
              s1.reconverts - s0.reconverts);
        /* the texture's last row, written by the renderer, then loaded again (physical addresses are GameCube-only) */
        gRam[a + 31 * 64 + 10] ^= 0x3C;
        gfx_tex_ram_written(K0(a) + 31 * 64 + 8, 8);
        load_texture_block(K0(a), G_IM_FMT_RGBA, G_IM_SIZ_16b, 32, 32, 0, 0, 0, 5, 5, 0, 0);
        drawDones = gDrawDones;
        gfx_tex_get_stats(&s0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        gfx_tex_get_stats(&s1);
        CHECK(s1.reconverts == s0.reconverts + 1 && gDrawDones == drawDones + 1, "rewrite: %u reconverts",
              s1.reconverts - s0.reconverts);
        /* a palette */
        palA = ram_random(16 * 2);
        a = ram_random(16 * 16 / 2);
        dp_set_tlut(G_TT_RGBA16);
        load_tlut(16, 256, K0(palA));
        load_multi_block_4b(K0(a), 0, 0, G_IM_FMT_CI, 16, 16, 0, 0, 0, 4, 4, 0, 0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        gRam[palA + 30] ^= 0x18;
        gfx_tex_ram_written(K0(palA) + 30, 2);
        load_tlut(16, 256, K0(palA));
        logs = gLogs;
        gfx_tex_get_stats(&s0);
        compare_tile(0, GX_TEXMAP0, GX_TF_RGB5A3, NULL);
        gfx_tex_get_stats(&s1);
        CHECK(s1.reconverts == s0.reconverts + 1 && gLogs == logs, "palette rewrite: %u reconverts",
              s1.reconverts - s0.reconverts);
    }
    test_end();
}

/* ================================================================================================ */
/* Tests: randomized loads and tiles against the model                                              */
/* ================================================================================================ */

/* Random but legal command sequences: a texture image of any size, 1-4 LOADBLOCK/LOADTILE/LOADTLUT with
 * random coordinates, dxt and load tile lines, then random render tiles (format, line, tmem, palette,
 * clamp/mirror/mask, tile size with fractions) and TLUT modes. Stays inside what the RDP defines and the
 * implementation supports: no 4-bit loads, no loads that wrap TMEM, no 16/32-bit texels with the TLUT on,
 * tiles under 256 rows. */
static void test_fuzz(void) {
    static const int kSiz[3] = { G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_32b };
    static const int kFmt[4] = { G_IM_FMT_RGBA, G_IM_FMT_IA, G_IM_FMT_I, G_IM_FMT_CI };
    static const uint32_t kTlut[3] = { G_TT_NONE, G_TT_RGBA16, G_TT_IA16 };
    int iter, compared = 0, slowCompared = 0, failures0 = sFailures;
    uint32_t pool = ram_random(1u << 20);

    test_begin("fuzz: random loads and tiles");
    for (iter = 0; iter < 3000; iter++) {
        int nloads = 1 + (int)(rnd() % 4), l, k, tmems[4], ntmems = 0;
        uint32_t tlut = kTlut[rnd() % 3];

        task_begin();
        sCmdLogN = 0;
        dp_set_tlut(tlut);
        for (l = 0; l < nloads; l++) {
            int kind = (int)(rnd() % 7); /* 0-2 block, 3-5 tile, 6 tlut */
            int siz = kSiz[rnd() % 3], bpt = ref_bytes(siz);
            int width = 1 + (int)(rnd() % 80), tmem, line;
            uint32_t a = pool + (rnd() % ((1u << 20) - 64 * 1024));

            if (kind == 6) {
                int count = 1 + (int)(rnd() % 256), pt = 256 + (int)(rnd() % 256);

                if (pt + count > 512) {
                    count = 512 - pt;
                }
                load_tlut(count, pt, K0(a));
                continue;
            }
            dp_set_timg(G_IM_FMT_RGBA, siz, width, K0(a));
            line = (rnd() & 3) ? 0 : (int)(rnd() % 8);
            if (kind < 3) {
                int uls = (int)(rnd() % 4), ult = (int)(rnd() % 3);
                int texels = 1 + (int)(rnd() % (siz == G_IM_SIZ_32b ? 512 : 1024));
                int words = (texels * bpt + 7) / 8;
                int dxt, lines, bytes;

                switch (rnd() % 4) {
                    case 0:
                        dxt = 0;
                        break;
                    case 1:
                        dxt = (int)(rnd() % 2048);
                        break;
                    default:
                        dxt = calc_dxt(1 + (int)(rnd() % 64), bpt);
                        break;
                }
                lines = ((words - 1) * dxt) >> 11;
                bytes = words * (siz == G_IM_SIZ_32b ? 4 : 8) + line * 8 * lines;
                if (line * lines > 511) {
                    line = 0;
                    bytes = words * (siz == G_IM_SIZ_32b ? 4 : 8);
                }
                tmem = (int)(rnd() % 512);
                if (tmem * 8 % (siz == G_IM_SIZ_32b ? 2048 : 4096) + bytes > (siz == G_IM_SIZ_32b ? 2048 : 4096)) {
                    tmem = 0;
                    if (bytes > (siz == G_IM_SIZ_32b ? 2048 : 4096)) {
                        continue;
                    }
                }
                if (siz == G_IM_SIZ_32b) {
                    tmem &= 0xFF;
                }
                dp_set_tile(G_IM_FMT_RGBA, siz, line, tmem, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
                dp_load_block(G_TX_LOADTILE, uls, ult, uls + texels - 1, dxt);
            } else {
                int sl = (int)(rnd() % 32), tl = (int)(rnd() % 16);
                int sh = sl + (int)(rnd() % 160), th = tl + (int)(rnd() % 80);
                int cols = (sh >> 2) - (sl >> 2) + 1, rows = (th >> 2) - (tl >> 2) + 1;
                int rowWords = (cols * bpt + 7) / 8;
                int step = siz == G_IM_SIZ_32b ? 4 : 8, limit = siz == G_IM_SIZ_32b ? 2048 : 4096;

                if ((sh >> 2) >= width) {
                    width = (sh >> 2) + 1;
                    dp_set_timg(G_IM_FMT_RGBA, siz, width, K0(a));
                }
                line = (rowWords * step + 7) / 8 + (int)(rnd() % 3) - (rnd() % 4 == 0);
                if (line < 1) {
                    line = 1;
                }
                if ((rows - 1) * line * 8 + rowWords * step > limit) {
                    continue;
                }
                tmem = (int)(rnd() % (limit / 8));
                if (tmem * 8 + (rows - 1) * line * 8 + rowWords * step > limit) {
                    tmem = 0;
                }
                dp_set_tile(G_IM_FMT_RGBA, siz, line, tmem, G_TX_LOADTILE, 0, 0, 0, 0, 0, 0, 0);
                dp_load_tile(G_TX_LOADTILE, sl, tl, sh, th);
            }
            tmems[ntmems++] = tmem;
        }
        if (ntmems == 0) {
            continue;
        }

        for (k = 0; k < 2; k++) {
            int fmt = kFmt[rnd() % 4], siz = (int)(rnd() % 4);
            int tile = k, w = 1 + (int)(rnd() % 48), h = 1 + (int)(rnd() % 48);
            int uls = (int)(rnd() % 32), ult = (int)(rnd() % 32);
            int lineBytes = siz == 0 ? (w + 1) / 2 : w * kLineBytes[siz];
            int line = (lineBytes + 7) / 8 + ((rnd() % 4 == 0) ? (int)(rnd() % 3) - 1 : 0);
            int tmem = tmems[rnd() % ntmems] + ((rnd() % 4 == 0) ? (int)(rnd() % 8) : 0);
            int cms = (int)(rnd() % 4), cmt = (int)(rnd() % 4);
            int masks = (rnd() % 3) ? (int)(rnd() % 7) : 0, maskt = (rnd() % 3) ? (int)(rnd() % 7) : 0;
            GfxTexBinding b;
            uint32_t slowBefore;

            if (siz >= G_IM_SIZ_16b && tlut != G_TT_NONE) {
                siz = (int)(rnd() % 2);
            }
            if (siz == G_IM_SIZ_32b) {
                fmt = G_IM_FMT_RGBA;
            }
            if (line < 0) {
                line = 0;
            }
            dp_set_tile(fmt, siz, line, tmem & 0x1FF, tile, (int)(rnd() % 16), cmt, maskt, 0, cms, masks, 0);
            dp_set_tile_size(tile, uls, ult, uls + (w - 1) * 4 + (int)(rnd() % 4),
                             ult + (h - 1) * 4 + (int)(rnd() % 4));
            slowBefore = slow_binds();
            memset(&b, 0, sizeof(b));
            if (!gfx_tex_bind(tile, k, &b)) {
                /* only acceptable when nothing was loaded where the tile reads */
                continue;
            }
            compare_tile(tile, k, -1, NULL);
            compared++;
            slowCompared += slow_binds() != slowBefore;
            if (iter % 16 == 0) {
                compare_sampling(tile, k);
            }
        }
        if (sFailures != failures0) {
            int c;

            printf("  fuzz iteration %d failed, commands:\n", iter);
            for (c = 0; c < sCmdLogN; c++) {
                printf("    %08X %08X\n", sCmdLog[c][0], sCmdLog[c][1]);
            }
            break;
        }
    }
    CHECK(compared > 3000, "only %d tiles compared", compared);
    printf("    %d tiles compared, %d through the slow path\n", compared, slowCompared);
    test_end();
}

/* The gbi.h load macros with random sizes, formats, TMEM addresses, palettes, wrap modes and tile offsets:
 * these are the layouts the fast path must get right. */
static void test_fuzz_macros(void) {
    static const int kFmt[5] = { G_IM_FMT_RGBA, G_IM_FMT_IA, G_IM_FMT_I, G_IM_FMT_CI, G_IM_FMT_I };
    int iter, compared = 0, fast = 0, failures0 = sFailures;
    uint32_t pool = ram_random(1u << 20);

    test_begin("fuzz: gbi macro load patterns");
    for (iter = 0; iter < 3000; iter++) {
        int k, n = 1 + (int)(rnd() % 2), tiles[2], ntiles = 0;
        uint32_t tlut = G_TT_NONE;

        task_begin();
        sCmdLogN = 0;
        if (rnd() % 3 == 0) {
            int count = (rnd() & 1) ? 256 : 16, pt = (count == 256) ? 256 : 256 + (int)(rnd() % 16) * 16;

            tlut = (rnd() & 1) ? G_TT_RGBA16 : G_TT_IA16;
            dp_set_tlut(tlut);
            load_tlut(count, pt, K0(pool + (rnd() % 65536) * 2));
        }
        for (k = 0; k < n; k++) {
            int siz = (int)(rnd() % 4), fmt = kFmt[rnd() % 5];
            /* (up to 64x64 texels of 32 bits, or 128x64 smaller ones, so that the slow path's scratch holds them) */
            int lw = 2 + (int)(rnd() % ((siz == G_IM_SIZ_32b) ? 5 : 6)), lh = (int)(rnd() % 7);
            int w = 1 << lw, h = 1 << lh;
            int tmemMax = (tlut != G_TT_NONE || siz == G_IM_SIZ_32b) ? 256 : 512;
            int bytes = siz == 0 ? w * h / 2 : w * h * ref_bytes(siz);
            int tmemBytes = siz == G_IM_SIZ_32b ? bytes / 2 : bytes;
            int tmem, pal = (int)(rnd() % 16), tile = k ? 1 + (int)(rnd() % 6) : 0;
            int cms = (int)(rnd() % 4), cmt = (int)(rnd() % 4);
            /* (masks past the data read TMEM wrapped) */
            int masks = (rnd() % 4) ? lw : (int)(rnd() % 7), maskt = (rnd() % 4) ? lh : (int)(rnd() % 7);
            int shifts = (rnd() % 4) ? 0 : (int)(rnd() % 16), shiftt = (rnd() % 4) ? 0 : (int)(rnd() % 16);
            uint32_t a = pool + (rnd() % 400000) * 2;
            int pattern = (int)(rnd() % 3);

            if (siz >= G_IM_SIZ_16b && tlut != G_TT_NONE) {
                siz = (int)(rnd() % 2);
                bytes = siz == 0 ? w * h / 2 : w * h;
                tmemBytes = bytes;
            }
            if (siz == G_IM_SIZ_32b) {
                fmt = G_IM_FMT_RGBA;
            }
            if (pattern != 0) {
                /* LOADTILE rows end on a 64-bit RAM word: 8 TMEM bytes, 4 for 32-bit texels */
                int rowBytes = siz == 0 ? (w + 1) / 2 : w * ref_bytes(siz);

                tmemBytes = h * (siz == G_IM_SIZ_32b ? (rowBytes + 7) / 8 * 4 : (rowBytes + 7) / 8 * 8);
                if (siz == G_IM_SIZ_32b) {
                    tmemBytes = (tmemBytes + 7) & ~7;
                }
            }
            if (tmemBytes > tmemMax * 8 || bytes < 8 || (siz == 0 && w < 16 && pattern == 0)) {
                continue;
            }
            tmem = (int)(rnd() % (tmemMax - (tmemBytes + 7) / 8 + 1));
            if (pattern == 0) {
                if (siz == 0) {
                    load_multi_block_4b(K0(a), tmem, tile, fmt, w, h, pal, cms, cmt, masks, maskt, shifts, shiftt);
                } else {
                    load_multi_block(K0(a), tmem, tile, fmt, siz, w, h, pal, cms, cmt, masks, maskt, shifts, shiftt,
                                     rnd() % 8 == 0);
                }
            } else {
                /* a sub-rectangle of a larger image, any width (gDPLoadMultiTile) */
                int iw = w + (int)(rnd() % 40), uls = (int)(rnd() % (iw - w + 1)), ult = (int)(rnd() % 8);
                int tw = 1 + (int)(rnd() % w);

                if (siz == 0) {
                    iw &= ~1;
                    uls &= ~1;
                    tw = (tw + 1) & ~1;
                    if (iw < uls + tw) {
                        iw = uls + tw;
                    }
                    load_multi_tile_4b(K0(a), tmem, tile, fmt, iw, uls, ult, uls + tw - 1, ult + h - 1, pal, cms, cmt,
                                       masks, maskt, shifts, shiftt);
                } else {
                    load_multi_tile(K0(a), tmem, tile, fmt, siz, iw, uls, ult, uls + tw - 1, ult + h - 1, pal, cms, cmt,
                                    masks, maskt, shifts, shiftt);
                }
            }
            if (rnd() % 3 == 0) {
                /* scrolled tile size, like Gfx_TwoTexScroll */
                int x = (int)(rnd() % 4096), y = (int)(rnd() % 4096);

                dp_set_tile_size(tile, x & 0xFFF, y & 0xFFF, (x + ((w - 1) << 2)) & 0xFFF,
                                 (y + ((h - 1) << 2)) & 0xFFF);
            }
            tiles[ntiles++] = tile;
        }
        for (k = 0; k < ntiles; k++) {
            uint32_t slowBefore = slow_binds();

            if (compare_tile(tiles[k], k, -1, NULL)) {
                compared++;
                fast += slow_binds() == slowBefore;
            }
            /* (shift 11 puts every sample on a texel edge, where the comparison skips it) */
            if (iter % 8 == 0 && R.tile[tiles[k]].shifts != 11 && R.tile[tiles[k]].shiftt != 11) {
                compare_sampling(tiles[k], k);
            }
        }
        if (sFailures != failures0) {
            int c;

            printf("  fuzz iteration %d failed, commands:\n", iter);
            for (c = 0; c < sCmdLogN; c++) {
                printf("    %08X %08X\n", sCmdLog[c][0], sCmdLog[c][1]);
            }
            break;
        }
    }
    CHECK(compared > 2000, "only %d tiles compared", compared);
    printf("    %d tiles compared, %d on the fast path\n", compared, fast);
    test_end();
}

/* ================================================================================================ */

int main(void) {
    GfxTexStats st;

    gRam = calloc(1, RAM_SIZE);
    if (gRam == NULL) {
        return 2;
    }

    /* The cache asks for 2 MiB and falls back: allow 300 KB, so it settles on 256 KB (and gets full) */
    gAllocLimit = 300 * 1024;
    gfx_tex_init();
    gfx_tex_get_stats(&st);
    test_begin("cache init fallback");
    CHECK(st.bytesTotal == 256 * 1024, "cache %u bytes", st.bytesTotal);
    test_end();

    test_formats();
    test_loadtile_layouts();
    test_tmem();
    test_wrap();
    test_rects();
    test_state();
    test_cache_basic();
    test_cache_pressure();
    test_cache_pinning();
    test_rect_strips();
    test_images();
    test_fuzz();
    test_fuzz_macros();

    test_begin("GX call protocol");
    CHECK(gGxErrors == 0, "%d GX protocol errors (alignment, sizes, invalidation, flush before DrawDone)", gGxErrors);
    test_end();

    gfx_tex_get_stats(&st);
    printf("\n%d checks, %d failed; cache: %u binds, %u hits, %u misses, %u reconverts, %u evictions, %u slow\n",
           sChecks, sFailures, st.binds, st.hits, st.misses, st.reconverts, st.evictions, st.slow);
    return sFailures ? 1 : 0;
}

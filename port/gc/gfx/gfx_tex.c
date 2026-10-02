/**
 * Textures for the N64 renderer (DESIGN.md, "Textures"):
 *
 *  - TMEM model: each G_LOADBLOCK / G_LOADTILE / G_LOADTLUT is recorded as "TMEM bytes [a, b) came from RAM
 *    address A with this layout". TMEM bytes are not emulated on the fast path.
 *  - Tile resolution: a tile is found through the newest load covering its TMEM range and decoded straight
 *    from RAM. Layouts that do not map to RAM rows one to one (several loads, a line or dxt that does not
 *    match the tile, odd row offsets) replay the loads into a scratch TMEM image and decode from that.
 *  - Conversion of every N64 format and size to tiled GX textures, reproducing the RDP's wrap, mirror,
 *    clamp and mask with GX wrap modes (masked regions inside a clamp are expanded on the CPU).
 *  - Texture cache: buddy allocator over one fixed block of memory, LRU eviction, content hashes checked
 *    once per task, so textures the game rewrites are converted again.
 *  - Whole images bound straight from RAM (gfx_tex_bind_image: S2DEX2 backgrounds, framebuffer textures), cached
 *    the same way.
 */
#include <string.h>
#include "gfx_internal.h"

/* ================================================================================================ */
/* Constants and types                                                                              */
/* ================================================================================================ */

#define TMEM_BYTES 4096
#define TMEM_MAX_LOADS 32

#define TEX_CACHE_SIZE (2u << 20) /* GFX_TEX_CACHE_SIZE in DESIGN.md */
#define TEX_CACHE_MIN (256u << 10)
#define TEX_MIN_ORDER 5 /* 32-byte blocks, the GX texture alignment */
#define TEX_MAX_ENTRIES 1024
#define TEX_HASH_SIZE 1024 /* power of two */
#define TEX_MAX_DIM 1024   /* GX limit, and the RDP's largest clamp */
#define TEX_MAX_TEXELS (256 * 256) /* tiles; images may use up to TEX_MAX_DIM squared */
#define TEX_SCRATCH_BYTES (16u << 10)
#define TEX_NONE 0xFFFFFFFFu

/* Texture sources are game RAM (MEM1). Anything else is a bad address: never read it. */
#define TEX_RAM_START 0x80000000u
#define TEX_RAM_END 0x81800000u

#define OM_TEXTLUT_ON (1u << 15) /* bit 15 of G_MDSFT_TEXTLUT: TLUT enabled; bit 14: IA16 palette */
#define OM_TEXTLUT_IA (1u << 14)
#define OM_BIND_BITS ((3u << G_MDSFT_CYCLETYPE) | (3u << G_MDSFT_TEXTLUT) | (3u << G_MDSFT_TEXTFILT))

enum { LOAD_BLOCK, LOAD_TILE, LOAD_TLUT };

typedef struct {
    uint16_t start, end; /* TMEM bytes [start, end). 32-bit loads count in the low half of TMEM, 2 bytes per
                            texel (red/green); blue/alpha go to the same offset in the high half */
    uint8_t kind;        /* LOAD_* */
    uint8_t siz;         /* G_IM_SIZ_* of the loaded texels */
    uint16_t dxt;        /* LOADBLOCK: 1.11 line increment per 64-bit word (selects the swapped odd lines) */
    uint16_t skip;       /* LOADBLOCK: the load tile's line (words), added to TMEM addresses once per line */
    uint16_t line;       /* LOADTILE: TMEM bytes between rows */
    uint16_t rowBytes;   /* LOADTILE: TMEM bytes written per row */
    uint16_t rows;       /* LOADBLOCK: 64-bit RAM words; LOADTILE: rows; LOADTLUT: palette entries */
    uint32_t src;        /* resolved RAM address of the first texel or palette entry */
    uint32_t stride;     /* LOADTILE: RAM bytes between rows */
} TmemLoad;

/* Conversions: N64 format/size (and TLUT mode) -> GX format */
enum {
    CONV_I4,       /* I4 -> GX I4 (also RGBA 4b, which the RDP reads as I4) */
    CONV_IA4,      /* IA4 (3-bit I, 1-bit A) -> GX IA4 */
    CONV_CI4_I8,   /* CI4 without TLUT: (palette << 4) | index as intensity -> GX I8 */
    CONV_CI4_RGBA, /* CI4 + RGBA16 TLUT -> GX RGB5A3 */
    CONV_CI4_IA,   /* CI4 + IA16 TLUT -> GX IA8 */
    CONV_I8,       /* I8 -> GX I8 (also RGBA 8b and CI8 without TLUT) */
    CONV_IA8,      /* IA8 (4-bit I, 4-bit A) -> GX IA4 */
    CONV_CI8_RGBA, /* CI8 + RGBA16 TLUT -> GX RGB5A3 */
    CONV_CI8_IA,   /* CI8 + IA16 TLUT -> GX IA8 */
    CONV_RGBA16,   /* RGBA5551 -> GX RGB5A3 */
    CONV_IA16,     /* IA16 (8-bit I, 8-bit A) -> GX IA8 (also the other 16-bit formats) */
    CONV_RGBA32,   /* RGBA8888 -> GX RGBA8 */
    CONV_YUV16,    /* YUV16 (U Y0 V Y1 per texel pair) -> GX RGBA8 (images only; TMEM splits YUV over both halves) */
    CONV_COUNT
};

static const uint8_t sConvSiz[CONV_COUNT] = {
    G_IM_SIZ_4b, G_IM_SIZ_4b, G_IM_SIZ_4b,  G_IM_SIZ_4b,  G_IM_SIZ_4b,  G_IM_SIZ_8b,  G_IM_SIZ_8b,
    G_IM_SIZ_8b, G_IM_SIZ_8b, G_IM_SIZ_16b, G_IM_SIZ_16b, G_IM_SIZ_32b, G_IM_SIZ_16b,
};
static const uint8_t sConvGxFmt[CONV_COUNT] = {
    GX_TF_I4,     GX_TF_IA4, GX_TF_I8,     GX_TF_RGB5A3, GX_TF_IA8,   GX_TF_I8,    GX_TF_IA4,
    GX_TF_RGB5A3, GX_TF_IA8, GX_TF_RGB5A3, GX_TF_IA8,    GX_TF_RGBA8, GX_TF_RGBA8,
};

/* Cache key: everything the converted texture depends on, apart from the bytes it was converted from */
#define KEY_SLOW 0x01     /* src is a content hash of texels gathered from the replayed TMEM */
#define KEY_SWAP_ODD 0x02 /* odd rows have their 32-bit words swapped in RAM (LOADBLOCK with dxt 0) */
#define KEY_PAL_ADDR 0x04 /* pal is the RAM address of the palette */
#define KEY_PAL_HASH 0x08 /* pal is a content hash of a palette gathered from the replayed TMEM */
#define KEY_IMAGE 0x10    /* gfx_tex_bind_image: src is the image's CPU address (hashed over its own row length) */

typedef struct {
    uint32_t src;           /* resolved RAM address of row 0 (or a content hash with KEY_SLOW) */
    uint32_t stride;        /* RAM bytes between rows */
    uint32_t pal;           /* palette address or hash; the palette number for CONV_CI4_I8 */
    uint16_t width, height; /* GX texture */
    uint16_t srcW, srcH;    /* N64 texels per row, rows read from TMEM */
    uint16_t palCount;      /* palette entries read at `pal` with KEY_PAL_ADDR (the others are 0) */
    uint8_t conv;
    uint8_t flags;      /* KEY_* */
    uint8_t mapS, mapT; /* 0, or a masked region expanded inside a clamp: 0x80 | mirror << 4 | mask */
    uint8_t pad[2];
} TexKey;

#define TEX_KEY_WORDS 7
_Static_assert(sizeof(TexKey) == TEX_KEY_WORDS * 4, "TexKey is hashed and compared as words");

typedef struct {
    TexKey key;
    uint32_t keyHash;
    uint32_t dataHash, palHash; /* source contents at the last check (address-keyed sources only) */
    uint32_t validFrame;        /* task in which the contents were last checked */
    uint32_t usedFrame;         /* task in which the texture was last bound */
    uint32_t offset;            /* texture memory: offset in the arena, 1 << order bytes */
    int16_t hashNext;           /* hash chain; free list link while unused */
    int16_t lruPrev, lruNext;   /* LRU list, most recent first */
    uint8_t order;
    uint8_t used;
} TexEntry;

/* Last binding per texture map: a repeated bind of an unchanged tile skips resolution and lookup. An image binding
 * (gfx_tex_bind_image) is recorded only to keep its texture from being evicted while the map holds it. */
typedef struct {
    bool valid;
    bool image;
    int16_t entry;
    uint32_t frame, tmemGen, omH;
    GfxTile tile;
    GXTexObj obj;
    GfxTexBinding binding;
} TexMemo;

typedef struct {
    int conv;
    uint16_t width, height;
    const uint8_t* rows; /* N64 texels, row y at rows + y * rowStride */
    uint32_t rowStride;
    uint32_t ci4pal;
    uint8_t* dst;
} TexJob;

enum {
    LOG_LOAD4B,
    LOG_TMEM_WRAP,
    LOG_FORMAT,
    LOG_TOO_BIG,
    LOG_SCRATCH,
    LOG_RAM,
    LOG_NO_FIT,
    LOG_IMAGE,
    LOG_UNLOADED,
    LOG_COUNT
};

/* ================================================================================================ */
/* State                                                                                            */
/* ================================================================================================ */

static TmemLoad sLoads[TMEM_MAX_LOADS]; /* oldest first */
static int sNumLoads;
static uint32_t sTmemGen = 1; /* changes with every load */

static uint8_t sVTmem[TMEM_BYTES] __attribute__((aligned(32))); /* replayed TMEM (slow path) */
static uint32_t sVTmemGen;

static uint8_t sScratch[TEX_SCRATCH_BYTES] __attribute__((aligned(32)));
static uint8_t sPalScratch[256 * 2];
static uint16_t sPal[256]; /* palette converted to the GX texel format */
static uint16_t sMapS[TEX_MAX_DIM + 8], sMapT[TEX_MAX_DIM + 8];

static bool sInit;
static uint8_t* sArena;
static uint32_t sArenaSize;
static int sMaxOrder;
static uint32_t sFreeHead[32];
static uint32_t sFreeBits[(TEX_CACHE_SIZE >> TEX_MIN_ORDER) / 32]; /* start of a free block, per 32 bytes */

static TexEntry sEntries[TEX_MAX_ENTRIES];
static int16_t sHash[TEX_HASH_SIZE];
static int16_t sLruHead, sLruTail, sFreeEntry;
static uint32_t sFrame = 1;
static bool sNeedInvalidate;
static TexMemo sMemo[2];
static GfxTexStats sStats;
static uint8_t sLogged[LOG_COUNT];

#define TEX_LOG_ONCE(id, ...)    \
    do {                         \
        if (!sLogged[id]) {      \
            sLogged[id] = 1;     \
            gc_log(__VA_ARGS__); \
        }                        \
    } while (0)

/* RAM addresses in cache keys: KSEG0 on the GameCube (the uncached mirror is the same memory); host tests use
 * their own pointers */
static uint32_t tex_key_addr(uint32_t addr) {
#ifdef GEKKO
    return (addr & 0x1FFFFFFF) | 0x80000000;
#else
    return addr;
#endif
}

static bool tex_ram_ok(uint32_t addr, uint32_t bytes) {
    if (addr >= TEX_RAM_START && addr < TEX_RAM_END && bytes <= TEX_RAM_END - addr) {
        return true;
    }
    TEX_LOG_ONCE(LOG_RAM, "gfx: tex: texture address %08X (+%u) is outside RAM", (unsigned int)addr,
                 (unsigned int)bytes);
    return false;
}

/* ================================================================================================ */
/* TMEM load records                                                                                */
/* ================================================================================================ */

static bool tmem_is32(const TmemLoad* ld) {
    return ld->siz == G_IM_SIZ_32b && ld->kind != LOAD_TLUT;
}

static bool range_overlap(uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1) {
    return a0 < b1 && b0 < a1;
}

static bool tmem_overlaps(const TmemLoad* ld, uint32_t a, uint32_t b) {
    if (range_overlap(ld->start, ld->end, a, b)) {
        return true;
    }
    return tmem_is32(ld) && range_overlap(ld->start + 2048, ld->end + 2048, a, b);
}

/* Newest load that wrote TMEM byte `a`, or -1 */
static int tmem_find(uint32_t a) {
    int i;

    for (i = sNumLoads - 1; i >= 0; i--) {
        if (tmem_overlaps(&sLoads[i], a, a + 1)) {
            return i;
        }
    }
    return -1;
}

/* Whether a load writes every TMEM byte of its [start, end) range, so that it hides older loads inside the range.
 * LOADBLOCK with a load tile line and LOADTILE with a line longer than the row leave gaps; a 32-bit LOADBLOCK whose
 * line parity changes on an odd RAM word writes two words to the same half of a TMEM word and leaves the other
 * half alone. */
static bool tmem_is_dense(const TmemLoad* ld) {
    switch (ld->kind) {
        case LOAD_BLOCK:
            if (ld->skip != 0) {
                return false;
            }
            return !tmem_is32(ld) || ld->dxt == 0 || (2048 % ld->dxt == 0 && ((2048 / ld->dxt) & 1) == 0);
        case LOAD_TILE:
            return ld->rows == 1 || ld->line <= ld->rowBytes;
        default:
            return true;
    }
}

static void tmem_add(TmemLoad* ld, uint32_t start, uint32_t bytes) {
    uint32_t limit = tmem_is32(ld) ? 2048 : TMEM_BYTES;
    bool dense;
    int i, n = 0;

    start &= limit - 1;
    if (start + bytes > limit) {
        /* The RDP wraps around TMEM; nothing in MM is expected to do that */
        TEX_LOG_ONCE(LOG_TMEM_WRAP, "gfx: tex: TMEM load at %u (%u bytes) wraps, truncated", (unsigned int)start,
                     (unsigned int)bytes);
        bytes = limit - start;
    }
    ld->start = start;
    ld->end = start + bytes;
    dense = tmem_is_dense(ld);

    /* Drop older loads that this one overwrites completely */
    for (i = 0; i < sNumLoads; i++) {
        const TmemLoad* old = &sLoads[i];

        if (dense && old->start >= ld->start && old->end <= ld->end && (!tmem_is32(old) || tmem_is32(ld))) {
            continue;
        }
        sLoads[n++] = *old;
    }
    if (n == TMEM_MAX_LOADS) {
        memmove(&sLoads[0], &sLoads[1], (TMEM_MAX_LOADS - 1) * sizeof(TmemLoad));
        n--;
    }
    sLoads[n++] = *ld;
    sNumLoads = n;
    sTmemGen++;
}

/* RAM bytes per texel of a loaded size (4-bit loads are not possible on the RDP) */
static uint32_t siz_ram_bytes(int siz) {
    return 1u << (siz - 1);
}

/* TMEM bytes filled per 64-bit RAM word: 32-bit texels are split over the two TMEM halves */
static uint32_t siz_tmem_step(int siz) {
    return (siz == G_IM_SIZ_32b) ? 4 : 8;
}

/* TMEM bytes per texel: 32-bit texels are split over the two TMEM halves */
static uint32_t siz_tmem_bytes(int siz) {
    return (siz == G_IM_SIZ_32b) ? 2 : siz_ram_bytes(siz);
}

void gfx_tex_load_block(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    const GfxTile* t = &gGfxRdp.tiles[tile & 7];
    int siz = gGfxRdp.texImageSiz;
    TmemLoad ld;

    if (siz == G_IM_SIZ_4b) {
        TEX_LOG_ONCE(LOG_LOAD4B, "gfx: tex: 4-bit LOADBLOCK/LOADTILE ignored");
        return;
    }
    if (gGfxRdp.texImageAddr == 0 || lrs < uls) {
        return;
    }
    memset(&ld, 0, sizeof(ld));
    ld.kind = LOAD_BLOCK;
    ld.siz = siz;
    ld.dxt = dxt;
    ld.skip = t->line;
    ld.rows = ((lrs - uls + 1) * siz_ram_bytes(siz) + 7) >> 3;
    /* uls/ult are whole texels and rows here, not 10.2 */
    ld.src = gGfxRdp.texImageAddr + (ult * gGfxRdp.texImageWidth + uls) * siz_ram_bytes(siz);
    /* Word i goes to tmem + i + line * ((i * dxt) >> 11): with a load tile line of 0 (the gbi.h macros), one
     * contiguous range; otherwise each line skips `line` words */
    tmem_add(&ld, t->tmem * 8, ld.rows * siz_tmem_step(siz) + ld.skip * 8 * (((ld.rows - 1) * dxt) >> 11));
}

void gfx_tex_load_tile(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    const GfxTile* t = &gGfxRdp.tiles[tile & 7];
    int siz = gGfxRdp.texImageSiz;
    uint32_t sl = uls >> 2, tl = ult >> 2, sh = lrs >> 2, th = lrt >> 2;
    TmemLoad ld;

    if (siz == G_IM_SIZ_4b) {
        TEX_LOG_ONCE(LOG_LOAD4B, "gfx: tex: 4-bit LOADBLOCK/LOADTILE ignored");
        return;
    }
    if (gGfxRdp.texImageAddr == 0 || sh < sl || th < tl) {
        return;
    }
    memset(&ld, 0, sizeof(ld));
    ld.kind = LOAD_TILE;
    ld.siz = siz;
    ld.line = t->line * 8;
    /* The RDP moves whole 64-bit words from RAM, so a row in TMEM ends on a word boundary */
    ld.rowBytes = (((sh - sl + 1) * siz_ram_bytes(siz) + 7) >> 3) * siz_tmem_step(siz);
    ld.rows = th - tl + 1;
    ld.stride = gGfxRdp.texImageWidth * siz_ram_bytes(siz);
    ld.src = gGfxRdp.texImageAddr + tl * ld.stride + sl * siz_ram_bytes(siz);
    tmem_add(&ld, t->tmem * 8, (ld.rows - 1) * ld.line + ld.rowBytes);
}

void gfx_tex_load_tlut(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    const GfxTile* t = &gGfxRdp.tiles[tile & 7];
    uint32_t sl = uls >> 2, sh = lrs >> 2;
    TmemLoad ld;

    if (gGfxRdp.texImageAddr == 0 || sh < sl) {
        return;
    }
    memset(&ld, 0, sizeof(ld));
    ld.kind = LOAD_TLUT;
    ld.siz = G_IM_SIZ_16b;
    ld.rows = sh - sl + 1;
    ld.src = gGfxRdp.texImageAddr + ((ult >> 2) * gGfxRdp.texImageWidth + sl) * 2;
    /* Each palette entry fills one 64-bit TMEM word (the RDP stores it four times) */
    tmem_add(&ld, t->tmem * 8, ld.rows * 8);
}

/* ------------------------------------------------------------------------------------------------ */
/* Replayed TMEM (slow path): the loads executed into a 4 KB image, then read like the RDP does      */
/* ------------------------------------------------------------------------------------------------ */

/* Store one 64-bit word read from RAM at TMEM byte `p` (8-byte aligned); `swap` exchanges its 32-bit halves,
 * which the RDP does for odd lines. 32-bit texels: red/green to the low half, blue/alpha to the high half,
 * 4 bytes each. */
static void vtmem_store(const uint8_t* s, uint32_t p, uint32_t swap, bool is32) {
    uint32_t k;

    if (is32) {
        uint32_t lo = (p ^ (swap << 2)) & 0x7FF;
        uint32_t hi = lo | 0x800;

        sVTmem[lo] = s[0];
        sVTmem[lo + 1] = s[1];
        sVTmem[lo + 2] = s[4];
        sVTmem[lo + 3] = s[5];
        sVTmem[hi] = s[2];
        sVTmem[hi + 1] = s[3];
        sVTmem[hi + 2] = s[6];
        sVTmem[hi + 3] = s[7];
        return;
    }
    for (k = 0; k < 8; k++) {
        sVTmem[(p + (k ^ (swap << 2))) & (TMEM_BYTES - 1)] = s[k];
    }
}

static void vtmem_replay(const TmemLoad* ld) {
    bool is32 = tmem_is32(ld);
    uint32_t step = is32 ? 4 : 8; /* TMEM bytes per 64-bit RAM word */
    const uint8_t* s;
    uint32_t i, r;

    switch (ld->kind) {
        case LOAD_BLOCK:
            if (!tex_ram_ok(ld->src, ld->rows * 8) || (s = gfx_addr(ld->src)) == NULL) {
                return;
            }
            for (i = 0; i < ld->rows; i++) {
                uint32_t line = (i * ld->dxt) >> 11;

                vtmem_store(s + i * 8, ld->start + i * step + ((ld->skip * line) & 0x1FF) * 8, line & 1, is32);
            }
            break;
        case LOAD_TILE:
            if (!tex_ram_ok(ld->src, (ld->rows - 1) * ld->stride + ld->rowBytes / step * 8) ||
                (s = gfx_addr(ld->src)) == NULL) {
                return;
            }
            for (r = 0; r < ld->rows; r++) {
                for (i = 0; i < ld->rowBytes / step; i++) {
                    vtmem_store(s + r * ld->stride + i * 8, ld->start + r * ld->line + i * step, r & 1, is32);
                }
            }
            break;
        default: /* LOAD_TLUT */
            if (!tex_ram_ok(ld->src, ld->rows * 2) || (s = gfx_addr(ld->src)) == NULL) {
                return;
            }
            for (i = 0; i < ld->rows; i++) {
                for (r = 0; r < 8; r += 2) {
                    sVTmem[(ld->start + i * 8 + r) & (TMEM_BYTES - 1)] = s[i * 2];
                    sVTmem[(ld->start + i * 8 + r + 1) & (TMEM_BYTES - 1)] = s[i * 2 + 1];
                }
            }
            break;
    }
}

static void vtmem_build(void) {
    int i;

    if (sVTmemGen == sTmemGen) {
        return;
    }
    memset(sVTmem, 0, sizeof(sVTmem));
    for (i = 0; i < sNumLoads; i++) {
        vtmem_replay(&sLoads[i]);
    }
    sVTmemGen = sTmemGen;
}

/* Bytes of one N64 texel row as stored in RAM */
static uint32_t row_ram_bytes(int siz, uint32_t w) {
    switch (siz) {
        case G_IM_SIZ_4b:
            return (w + 1) >> 1;
        case G_IM_SIZ_8b:
            return w;
        case G_IM_SIZ_16b:
            return w * 2;
        default:
            return w * 4;
    }
}

/* Read a tile's texels from the replayed TMEM into dst (srcH rows of row_ram_bytes), as the RDP addresses
 * them: row y starts at tmem + line * y (words, wrapping at `limit` bytes) and odd rows read with swapped
 * words. */
static void vtmem_gather(uint8_t* dst, uint32_t tmem, uint32_t line, int siz, uint32_t srcW, uint32_t srcH,
                         uint32_t limit) {
    uint32_t rowBytes = row_ram_bytes(siz, srcW);
    uint32_t x, y;

    for (y = 0; y < srcH; y++, dst += rowBytes) {
        uint32_t base = (((line * y) & 0x1FF) + tmem) * 8;
        uint32_t swap = (y & 1) << 2;

        if (siz == G_IM_SIZ_32b) {
            for (x = 0; x < srcW; x++) {
                uint32_t p = ((base + x * 2) ^ swap) & 0x7FF;

                dst[x * 4] = sVTmem[p];
                dst[x * 4 + 1] = sVTmem[p + 1];
                dst[x * 4 + 2] = sVTmem[p | 0x800];
                dst[x * 4 + 3] = sVTmem[(p | 0x800) + 1];
            }
        } else {
            for (x = 0; x < rowBytes; x++) {
                dst[x] = sVTmem[(base + (x ^ swap)) & (limit - 1)];
            }
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Fast path: the tile's rows are rows of one load, directly in RAM                                 */
/* ------------------------------------------------------------------------------------------------ */

/* LOADBLOCK: the RDP swaps the 32-bit halves of the words of odd lines, a line being counted by adding dxt
 * per 64-bit word (line = (word * dxt) >> 11). The tile reads odd rows swapped back. The tile maps to RAM
 * rows when each row's words all fall on one load line of the same parity. */
static bool block_rows_linear(const TmemLoad* ld, uint32_t rel, uint32_t lineBytes, uint32_t rowBytes, uint32_t rows,
                              bool is32, bool* swapOdd) {
    uint32_t qs = is32 ? 2 : 3; /* TMEM bytes per RAM word: 4 or 8 */
    uint32_t lineQ = lineBytes >> qs, q0 = rel >> qs, rowQ = rowBytes >> qs;
    uint32_t dxt = ld->dxt;
    uint32_t y;

    *swapOdd = false;
    if (dxt == 0) {
        /* Nothing swapped on load: the image in RAM already has its odd rows swapped (the ...S macros) */
        *swapOdd = rows > 1;
        return true;
    }
    if (rows > 1 && lineQ * dxt == 2048 && q0 % lineQ == 0 && !((q0 / lineQ) & 1)) {
        return true; /* the usual case: dxt from CALC_DXT for a power-of-two line */
    }
    for (y = 0; y < rows; y++) {
        uint32_t qa = q0 + y * lineQ;
        uint32_t fa = (qa * dxt) >> 11;
        uint32_t fb = ((qa + rowQ - 1) * dxt) >> 11;

        if (fa != fb || ((fa ^ y) & 1)) {
            return false;
        }
    }
    return true;
}

/* Find the RAM rows of a tile: TMEM bytes [tmemAddr, ...) with rows of rowBytes every lineBytes. */
static bool tex_find_fast(uint32_t tmemAddr, uint32_t lineBytes, uint32_t rowBytes, uint32_t rows, bool is32,
                          uint32_t limit, uint32_t* src, uint32_t* stride, bool* swapOdd) {
    uint32_t span = (rows - 1) * lineBytes + rowBytes;
    uint32_t mul = is32 ? 2 : 1; /* RAM bytes per TMEM byte */
    const TmemLoad* ld;
    uint32_t rel;
    int i, j;

    if (rows > 1 && rowBytes > lineBytes) {
        return false; /* rows overlap, or read past the line into the next one */
    }
    if (tmemAddr + span > limit) {
        return false; /* wraps around TMEM */
    }
    i = tmem_find(tmemAddr);
    if (i < 0) {
        return false;
    }
    ld = &sLoads[i];
    if (tmem_is32(ld) != is32 || ld->kind == LOAD_TLUT || ld->start > tmemAddr || tmemAddr + span > ld->end) {
        return false;
    }
    for (j = i + 1; j < sNumLoads; j++) {
        if (tmem_overlaps(&sLoads[j], tmemAddr, tmemAddr + span) ||
            (is32 && tmem_overlaps(&sLoads[j], tmemAddr + 2048, tmemAddr + span + 2048))) {
            return false;
        }
    }
    rel = tmemAddr - ld->start;

    if (ld->kind == LOAD_BLOCK) {
        if (ld->skip != 0 || !block_rows_linear(ld, rel, lineBytes, rowBytes, rows, is32, swapOdd)) {
            return false;
        }
        *src = ld->src + rel * mul;
        *stride = lineBytes * mul;
        return true;
    }

    /* LOADTILE: odd load rows were swapped on load, so the tile must start on an even load row */
    if (ld->line == 0 || ld->rowBytes > ld->line || (rows > 1 && lineBytes != ld->line)) {
        return false;
    }
    if ((rel % ld->line) + rowBytes > ld->rowBytes || ((rel / ld->line) & 1)) {
        return false;
    }
    *src = ld->src + (rel / ld->line) * ld->stride + (rel % ld->line) * mul;
    *stride = ld->stride;
    *swapOdd = false;
    return true;
}

/* Whether any load wrote TMEM bytes [a, a + n) (wrapping at limit), or their high-half copy for 32-bit tiles */
static bool tex_any_load(uint32_t a, uint32_t n, bool is32, uint32_t limit) {
    int i;

    if (n >= limit) {
        return sNumLoads != 0;
    }
    for (i = 0; i < sNumLoads; i++) {
        const TmemLoad* ld = &sLoads[i];

        if (tmem_overlaps(ld, a, a + n) || (a + n > limit && tmem_overlaps(ld, 0, a + n - limit)) ||
            (is32 && tmem_overlaps(ld, a + 2048, a + n + 2048))) {
            return true;
        }
    }
    return false;
}

/* Palette entries [first, first + n) of the TLUT (TMEM words 256 + first...): the first `count` of them in one TLUT
 * load, the others never loaded (0, as in the replayed TMEM). A CI8 texture often comes with a TLUT of fewer than
 * 256 entries. */
static bool tex_find_pal(uint32_t first, uint32_t n, uint32_t* addr, uint32_t* count) {
    uint32_t a = (256 + first) * 8, b = a + n * 8, loaded;
    const TmemLoad* ld;
    int i, j;

    i = tmem_find(a);
    if (i < 0) {
        return false;
    }
    ld = &sLoads[i];
    if (ld->kind != LOAD_TLUT) {
        return false;
    }
    loaded = (ld->end < b) ? ld->end : b;
    for (j = 0; j < sNumLoads; j++) {
        /* Newer loads anywhere in the palette, older ones past the end of this load */
        if ((j > i && tmem_overlaps(&sLoads[j], a, b)) ||
            (j < i && loaded < b && tmem_overlaps(&sLoads[j], loaded, b))) {
            return false;
        }
    }
    *addr = ld->src + ((a - ld->start) >> 3) * 2;
    *count = (loaded - a) >> 3;
    return true;
}

/* ================================================================================================ */
/* Hashing                                                                                          */
/* ================================================================================================ */

static uint32_t tex_hash(const uint8_t* p, uint32_t n, uint32_t h) {
    uint32_t h2 = h ^ 0x85EBCA6B;
    uint32_t a, b;

    while (n >= 8) {
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        h = (h ^ a) * 0x9E3779B1;
        h2 = (h2 ^ b) * 0x85EBCA77;
        h = (h << 13) | (h >> 19);
        h2 = (h2 << 15) | (h2 >> 17);
        p += 8;
        n -= 8;
    }
    while (n-- != 0) {
        h = (h ^ *p++) * 0x01000193;
    }
    h ^= h2 * 0xC2B2AE35;
    return h ^ (h >> 16);
}

static uint32_t tex_hash_rows(const uint8_t* p, uint32_t stride, uint32_t rowBytes, uint32_t rows) {
    uint32_t h = 0x165667B1 ^ rowBytes, y;

    if (stride == rowBytes) {
        return tex_hash(p, rowBytes * rows, h);
    }
    for (y = 0; y < rows; y++) {
        h = tex_hash(p + y * stride, rowBytes, h);
    }
    return h;
}

static uint32_t tex_hash_ram(uint32_t addr, uint32_t stride, uint32_t rowBytes, uint32_t rows) {
    const uint8_t* p;

    if (!tex_ram_ok(addr, (rows - 1) * stride + rowBytes) || (p = gfx_addr(addr)) == NULL) {
        return 0;
    }
    return tex_hash_rows(p, stride, rowBytes, rows);
}

static uint32_t key_hash(const TexKey* k) {
    uint32_t w[TEX_KEY_WORDS], h = 0x811C9DC5;
    int i;

    memcpy(w, k, sizeof(w));
    for (i = 0; i < TEX_KEY_WORDS; i++) {
        h = (h ^ w[i]) * 0x01000193;
        h ^= h >> 15;
    }
    return h;
}

/* ================================================================================================ */
/* Texture memory: buddy allocator over the arena                                                   */
/* ================================================================================================ */

/* A free block holds its links: word 0 next, word 1 previous, word 2 order */
static uint32_t* arena_words(uint32_t off) {
    return (uint32_t*)(void*)(sArena + off);
}

static bool freebit_get(uint32_t off) {
    uint32_t u = off >> TEX_MIN_ORDER;

    return (sFreeBits[u >> 5] >> (u & 31)) & 1;
}

static void freebit_set(uint32_t off, bool on) {
    uint32_t u = off >> TEX_MIN_ORDER;

    if (on) {
        sFreeBits[u >> 5] |= 1u << (u & 31);
    } else {
        sFreeBits[u >> 5] &= ~(1u << (u & 31));
    }
}

static void buddy_push(int k, uint32_t off) {
    uint32_t* b = arena_words(off);

    b[0] = sFreeHead[k];
    b[1] = TEX_NONE;
    b[2] = k;
    if (sFreeHead[k] != TEX_NONE) {
        arena_words(sFreeHead[k])[1] = off;
    }
    sFreeHead[k] = off;
    freebit_set(off, true);
}

static void buddy_unlink(int k, uint32_t off) {
    uint32_t* b = arena_words(off);

    if (b[1] != TEX_NONE) {
        arena_words(b[1])[0] = b[0];
    } else {
        sFreeHead[k] = b[0];
    }
    if (b[0] != TEX_NONE) {
        arena_words(b[0])[1] = b[1];
    }
    freebit_set(off, false);
}

static uint32_t buddy_alloc(int k) {
    uint32_t off;
    int o;

    for (o = k; o <= sMaxOrder && sFreeHead[o] == TEX_NONE; o++) {}
    if (o > sMaxOrder) {
        return TEX_NONE;
    }
    off = sFreeHead[o];
    buddy_unlink(o, off);
    while (o > k) {
        o--;
        buddy_push(o, off + (1u << o));
    }
    return off;
}

static void buddy_free(int k, uint32_t off) {
    while (k < sMaxOrder) {
        uint32_t buddy = off ^ (1u << k);

        if (!freebit_get(buddy) || arena_words(buddy)[2] != (uint32_t)k) {
            break;
        }
        buddy_unlink(k, buddy);
        off &= ~(1u << k);
        k++;
    }
    buddy_push(k, off);
}

/* ================================================================================================ */
/* Cache entries                                                                                    */
/* ================================================================================================ */

static void lru_unlink(int e) {
    TexEntry* en = &sEntries[e];

    if (en->lruPrev >= 0) {
        sEntries[en->lruPrev].lruNext = en->lruNext;
    } else {
        sLruHead = en->lruNext;
    }
    if (en->lruNext >= 0) {
        sEntries[en->lruNext].lruPrev = en->lruPrev;
    } else {
        sLruTail = en->lruPrev;
    }
    en->lruPrev = en->lruNext = -1;
}

static void lru_push_front(int e) {
    TexEntry* en = &sEntries[e];

    en->lruPrev = -1;
    en->lruNext = sLruHead;
    if (sLruHead >= 0) {
        sEntries[sLruHead].lruPrev = e;
    } else {
        sLruTail = e;
    }
    sLruHead = e;
}

static int cache_find(const TexKey* key, uint32_t h) {
    int e;

    for (e = sHash[h & (TEX_HASH_SIZE - 1)]; e >= 0; e = sEntries[e].hashNext) {
        if (sEntries[e].keyHash == h && memcmp(&sEntries[e].key, key, sizeof(*key)) == 0) {
            return e;
        }
    }
    return -1;
}

static void cache_evict(int e) {
    TexEntry* en = &sEntries[e];
    int16_t* link = &sHash[en->keyHash & (TEX_HASH_SIZE - 1)];

    while (*link != e) {
        link = &sEntries[*link].hashNext;
    }
    *link = en->hashNext;
    lru_unlink(e);
    buddy_free(en->order, en->offset);
    sStats.bytesUsed -= 1u << en->order;
    sStats.entries--;
    sStats.evictions++;
    en->used = 0;
    en->hashNext = sFreeEntry;
    sFreeEntry = e;
    if (sMemo[0].entry == e) {
        sMemo[0].valid = false;
    }
    if (sMemo[1].entry == e) {
        sMemo[1].valid = false;
    }
}

/* The texture currently loaded in a GX texture map for this task: binding the other map for the same draw must not
 * free it (its memory would be overwritten before the draw reads it) */
static bool cache_pinned(int e) {
    return (sMemo[0].valid && sMemo[0].frame == sFrame && sMemo[0].entry == e) ||
           (sMemo[1].valid && sMemo[1].frame == sFrame && sMemo[1].entry == e);
}

/* Evict the least recently used texture. Draws queued in this task may still read it: wait for GX first. */
static bool cache_evict_lru(bool* synced) {
    int e = sLruTail;

    while (e >= 0 && cache_pinned(e)) {
        e = sEntries[e].lruPrev;
    }
    if (e < 0) {
        return false;
    }
    if (sEntries[e].usedFrame == sFrame && !*synced) {
        gfx_gx_flush();
        GX_DrawDone();
        *synced = true;
        sStats.syncs++;
    }
    cache_evict(e);
    return true;
}

static int cache_insert(const TexKey* key, uint32_t h, uint32_t size) {
    bool synced = false;
    uint32_t off;
    TexEntry* en;
    int k = TEX_MIN_ORDER, e;

    while ((1u << k) < size) {
        k++;
    }
    if (k > sMaxOrder) {
        return -1;
    }
    while (sFreeEntry < 0) {
        if (!cache_evict_lru(&synced)) {
            return -1;
        }
    }
    e = sFreeEntry;
    sFreeEntry = sEntries[e].hashNext;
    while ((off = buddy_alloc(k)) == TEX_NONE) {
        if (!cache_evict_lru(&synced)) {
            sEntries[e].hashNext = sFreeEntry;
            sFreeEntry = e;
            return -1;
        }
    }
    en = &sEntries[e];
    memset(en, 0, sizeof(*en));
    en->key = *key;
    en->keyHash = h;
    en->offset = off;
    en->order = k;
    en->used = 1;
    en->hashNext = sHash[h & (TEX_HASH_SIZE - 1)];
    sHash[h & (TEX_HASH_SIZE - 1)] = e;
    en->lruPrev = en->lruNext = -1;
    lru_push_front(e);
    sStats.bytesUsed += 1u << k;
    sStats.entries++;
    return e;
}

/* ================================================================================================ */
/* Conversion to GX textures                                                                        */
/* ================================================================================================ */

static uint32_t rgba16_to_rgb5a3(uint32_t c) {
    if (c & 1) {
        return 0x8000 | (c >> 1); /* opaque: 1 RRRRR GGGGG BBBBB */
    }
    /* transparent: 0 AAA RRRR GGGG BBBB with alpha 0 */
    return (((c >> 12) & 0xF) << 8) | (((c >> 7) & 0xF) << 4) | ((c >> 2) & 0xF);
}

static uint32_t ia16_to_ia8(uint32_t c) {
    return ((c & 0xFF) << 8) | (c >> 8); /* N64: intensity, alpha; GX: alpha, intensity */
}

static uint32_t clamp_u8(int32_t v) {
    return (v < 0) ? 0 : (v > 255) ? 255 : (uint32_t)v;
}

/* YUV to opaque RGBA8 with the RDP's conversion and libultra's default coefficients (G_CV_K0..K3, /128) */
static uint32_t yuv_to_rgba(uint32_t y, int32_t u, int32_t v) {
    int32_t r = (int32_t)y + ((175 * v + 64) >> 7);
    int32_t g = (int32_t)y + ((-43 * u - 89 * v + 64) >> 7);
    int32_t b = (int32_t)y + ((222 * u + 64) >> 7);

    return (clamp_u8(r) << 24) | (clamp_u8(g) << 16) | (clamp_u8(b) << 8) | 0xFF;
}

/* One N64 texel of `row`, converted to the GX texel value (4, 8, 16 bits, or RGBA for RGBA8) */
static inline __attribute__((always_inline)) uint32_t tex_fetch(int conv, const uint8_t* row, uint32_t x,
                                                                uint32_t ci4pal) {
    uint32_t v;

    switch (conv) {
        case CONV_I4:
        case CONV_IA4:
        case CONV_CI4_I8:
        case CONV_CI4_RGBA:
        case CONV_CI4_IA:
            v = row[x >> 1];
            v = (x & 1) ? (v & 0xF) : (v >> 4);
            if (conv == CONV_IA4) {
                /* 3-bit intensity to 4 bits, 1-bit alpha to 4 bits */
                return ((v & 1) ? 0xF0 : 0x00) | (v & 0xE) | (v >> 3);
            }
            if (conv == CONV_CI4_I8) {
                return (ci4pal << 4) | v;
            }
            if (conv == CONV_I4) {
                return v;
            }
            return sPal[v];
        case CONV_I8:
            return row[x];
        case CONV_IA8:
            v = row[x];
            return ((v & 0xF) << 4) | (v >> 4); /* N64: intensity high; GX IA4: alpha high */
        case CONV_CI8_RGBA:
        case CONV_CI8_IA:
            return sPal[row[x]];
        case CONV_RGBA16:
            return rgba16_to_rgb5a3((row[x * 2] << 8) | row[x * 2 + 1]);
        case CONV_IA16:
            return ia16_to_ia8((row[x * 2] << 8) | row[x * 2 + 1]);
        case CONV_YUV16: {
            const uint8_t* p = row + (x & ~1u) * 2;

            return yuv_to_rgba(p[1 + (x & 1) * 2], (int32_t)p[0] - 128, (int32_t)p[2] - 128);
        }
        default: /* CONV_RGBA32 */
            return ((uint32_t)row[x * 4] << 24) | (row[x * 4 + 1] << 16) | (row[x * 4 + 2] << 8) | row[x * 4 + 3];
    }
}

static inline const uint8_t* job_row(const TexJob* j, uint32_t y) {
    if (y >= j->height) {
        y = j->height - 1; /* block padding, never sampled */
    }
    return j->rows + sMapT[y] * j->rowStride;
}

/* Write the GX texture in its tiled layout: blocks of 32 bytes (8x8 texels at 4 bits, 8x4 at 8 bits, 4x4 at
 * 16 bits) in rows of blocks; RGBA8 blocks are 4x4 texels in 64 bytes, alpha/red pairs then green/blue. */
static inline __attribute__((always_inline)) void tex_convert(const TexJob* j, int conv) {
    int gxFmt = sConvGxFmt[conv];
    int bw = (gxFmt == GX_TF_I4 || gxFmt == GX_TF_I8 || gxFmt == GX_TF_IA4) ? 8 : 4;
    int bh = (gxFmt == GX_TF_I4) ? 8 : 4;
    uint8_t* d = j->dst;
    const uint8_t* r[8];
    uint32_t bx, by;
    int i, k;

    for (by = 0; by < j->height; by += bh) {
        for (i = 0; i < bh; i++) {
            r[i] = job_row(j, by + i);
        }
        for (bx = 0; bx < j->width; bx += bw) {
            const uint16_t* ms = &sMapS[bx];

            if (gxFmt == GX_TF_I4) {
                for (i = 0; i < 8; i++) {
                    for (k = 0; k < 8; k += 2) {
                        *d++ = (tex_fetch(conv, r[i], ms[k], j->ci4pal) << 4) |
                               tex_fetch(conv, r[i], ms[k + 1], j->ci4pal);
                    }
                }
            } else if (gxFmt == GX_TF_I8 || gxFmt == GX_TF_IA4) {
                for (i = 0; i < 4; i++) {
                    for (k = 0; k < 8; k++) {
                        *d++ = tex_fetch(conv, r[i], ms[k], j->ci4pal);
                    }
                }
            } else if (gxFmt == GX_TF_RGBA8) {
                for (i = 0; i < 4; i++) {
                    for (k = 0; k < 4; k++) {
                        uint32_t v = tex_fetch(conv, r[i], ms[k], j->ci4pal);
                        uint8_t* p = d + (i * 4 + k) * 2;

                        p[0] = v;        /* A */
                        p[1] = v >> 24;  /* R */
                        p[32] = v >> 16; /* G */
                        p[33] = v >> 8;  /* B */
                    }
                }
                d += 64;
            } else {
                for (i = 0; i < 4; i++) {
                    for (k = 0; k < 4; k++) {
                        uint32_t v = tex_fetch(conv, r[i], ms[k], j->ci4pal);

                        d[0] = v >> 8;
                        d[1] = v;
                        d += 2;
                    }
                }
            }
        }
    }
}

static void tex_run(const TexJob* j) {
    switch (j->conv) {
#define TEX_CASE(c)        \
    case c:                \
        tex_convert(j, c); \
        break;
        TEX_CASE(CONV_I4)
        TEX_CASE(CONV_IA4)
        TEX_CASE(CONV_CI4_I8)
        TEX_CASE(CONV_CI4_RGBA)
        TEX_CASE(CONV_CI4_IA)
        TEX_CASE(CONV_I8)
        TEX_CASE(CONV_IA8)
        TEX_CASE(CONV_CI8_RGBA)
        TEX_CASE(CONV_CI8_IA)
        TEX_CASE(CONV_RGBA16)
        TEX_CASE(CONV_IA16)
        TEX_CASE(CONV_RGBA32)
        TEX_CASE(CONV_YUV16)
#undef TEX_CASE
    }
}

static uint32_t gx_tex_bytes(int gxFmt, uint32_t w, uint32_t h) {
    uint32_t bw = (gxFmt == GX_TF_I4 || gxFmt == GX_TF_I8 || gxFmt == GX_TF_IA4) ? 8 : 4;
    uint32_t bh = (gxFmt == GX_TF_I4) ? 8 : 4;

    return ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * (gxFmt == GX_TF_RGBA8 ? 64 : 32);
}

/* GX texel coordinate X -> N64 texel coordinate (relative to the tile) for one axis */
static void tex_build_map(uint16_t* map, uint32_t size, uint8_t mode) {
    uint32_t x, mask = mode & 0xF;
    uint32_t pad = (size + 7) & ~7u;

    for (x = 0; x < pad; x++) {
        uint32_t s = (x < size) ? x : size - 1;

        if (mode != 0) {
            /* masked region inside a clamp: the RDP wraps (or mirrors) s at the mask, then clamps */
            uint32_t m = s & ((1u << mask) - 1);

            if ((mode & 0x10) && (s & (1u << mask))) {
                m = ((1u << mask) - 1) - m;
            }
            s = m;
        }
        map[x] = s;
    }
}

/* Which conversion the RDP's texel decoding amounts to for a tile */
static int tex_conv(int fmt, int siz, uint32_t omH) {
    bool tlut = (omH & OM_TEXTLUT_ON) != 0;
    bool ia = (omH & OM_TEXTLUT_IA) != 0;

    switch (siz) {
        case G_IM_SIZ_4b:
            /* With the TLUT on, every 4- and 8-bit texel is a palette index */
            if (tlut) {
                return ia ? CONV_CI4_IA : CONV_CI4_RGBA;
            }
            if (fmt == G_IM_FMT_CI) {
                return CONV_CI4_I8;
            }
            if (fmt == G_IM_FMT_IA) {
                return CONV_IA4;
            }
            if (fmt != G_IM_FMT_I && fmt != G_IM_FMT_RGBA) {
                TEX_LOG_ONCE(LOG_FORMAT, "gfx: tex: unusual texture format %d/%d", fmt, siz);
            }
            return CONV_I4;
        case G_IM_SIZ_8b:
            if (tlut) {
                return ia ? CONV_CI8_IA : CONV_CI8_RGBA;
            }
            if (fmt == G_IM_FMT_IA) {
                return CONV_IA8;
            }
            if (fmt != G_IM_FMT_I && fmt != G_IM_FMT_CI && fmt != G_IM_FMT_RGBA) {
                TEX_LOG_ONCE(LOG_FORMAT, "gfx: tex: unusual texture format %d/%d", fmt, siz);
            }
            return CONV_I8;
        case G_IM_SIZ_16b:
            if (tlut) {
                /* The RDP would use the texel's high byte as a palette index: not expected in MM */
                TEX_LOG_ONCE(LOG_FORMAT, "gfx: tex: TLUT with 16/32-bit texels not supported");
            }
            if (fmt == G_IM_FMT_RGBA) {
                return CONV_RGBA16;
            }
            if (fmt != G_IM_FMT_IA) {
                TEX_LOG_ONCE(LOG_FORMAT, "gfx: tex: unusual texture format %d/%d", fmt, siz);
            }
            return CONV_IA16;
        default:
            if (fmt != G_IM_FMT_RGBA || tlut) {
                TEX_LOG_ONCE(LOG_FORMAT, "gfx: tex: unusual texture format %d/%d", fmt, siz);
            }
            return CONV_RGBA32;
    }
}

/* One axis of a tile (DESIGN.md "Wrap"). The RDP computes s - uls, clamps it to the tile size if clamping is
 * on (or there is no mask), then mirrors and masks it. Map that onto a GX texture of `size` texels read from
 * `srcSize` texels of TMEM:
 *   no mask                 CLAMP, the tile size
 *   mask, no clamp          REPEAT or MIRROR, the (power-of-two) mask size
 *   mask, clamp, tile fits  CLAMP, the tile size (the mask never applies)
 *   mask, clamp, larger     CLAMP, the tile size, the masked region repeated/mirrored into it on the CPU */
static void tex_axis(uint32_t ul, uint32_t lr, uint32_t mask, uint32_t cm, uint16_t* size, uint16_t* srcSize,
                     uint8_t* wrap, uint8_t* map) {
    uint32_t tileSize = (((lr >> 2) - (ul >> 2)) & 0x3FF) + 1;
    uint32_t maskSize;

    *map = 0;
    if (mask > 10) {
        mask = 10;
    }
    if (mask == 0) {
        *size = *srcSize = tileSize;
        *wrap = GX_CLAMP;
        return;
    }
    maskSize = 1u << mask;
    if (!(cm & G_TX_CLAMP)) {
        *size = *srcSize = maskSize;
        *wrap = (cm & G_TX_MIRROR) ? GX_MIRROR : GX_REPEAT;
        return;
    }
    *wrap = GX_CLAMP;
    *size = tileSize;
    if (tileSize <= maskSize) {
        *srcSize = tileSize;
        return;
    }
    *srcSize = maskSize;
    *map = 0x80 | ((cm & G_TX_MIRROR) ? 0x10 : 0) | mask;
}

static float shift_scale(uint32_t shift) {
    if (shift == 0) {
        return 1.0f;
    }
    if (shift <= 10) {
        return 1.0f / (float)(1u << shift);
    }
    return (float)(1u << (16 - shift));
}

/* ================================================================================================ */
/* Binding                                                                                          */
/* ================================================================================================ */

static void tex_load_obj(GXTexObj* obj, int texMap) {
    if (sNeedInvalidate) {
        /* Texture memory was rewritten: drop what the GX texture cache holds for it */
        GX_InvalidateTexAll();
        sNeedInvalidate = false;
    }
    GX_LoadTexObj(obj, texMap);
}

/* Palette for a CI conversion: `n` RGBA16 or IA16 entries from p, then 0 up to `total` */
static void tex_convert_pal(const uint8_t* p, uint32_t n, uint32_t total, int conv) {
    bool ia = conv == CONV_CI4_IA || conv == CONV_CI8_IA;
    uint32_t i;

    for (i = 0; i < total; i++) {
        uint32_t c = (i < n) ? ((p[i * 2] << 8) | p[i * 2 + 1]) : 0;

        sPal[i] = ia ? ia16_to_ia8(c) : rgba16_to_rgb5a3(c);
    }
}

/* A texture converted again in the middle of a task (gfx_tex_ram_written): draws already queued may still read the
 * old texels from the same memory */
static void tex_sync_rewrite(void) {
    gfx_gx_flush();
    GX_DrawDone();
    sStats.syncs++;
}

static bool tex_fail(GfxTexBinding* out) {
    out->valid = false;
    sStats.failures++;
    return false;
}

bool gfx_tex_bind(int tile, int texMap, GfxTexBinding* out) {
    const GfxTile* t;
    uint32_t omH;
    TexMemo* memo = NULL;
    TexKey key;
    int conv, siz, e;
    bool is32, fast, swapOdd = false, linear, copy, convert = false;
    uint32_t limit;
    uint8_t wrapS, wrapT;
    uint16_t width, height, srcW, srcH;
    uint32_t tmemAddr, lineBytes, tmemRow, ramRow, src = 0, stride = 0, palFirst = 0, palCount = 0, palAddr = 0;
    uint32_t palRam = 0;
    uint32_t h, size, mul;
    GXTexObj obj;
    TexEntry* en;

    sStats.binds++;
    out->valid = false;
    if (!sInit) {
        gfx_tex_init();
    }
    if (sArena == NULL) {
        return tex_fail(out);
    }
    t = &gGfxRdp.tiles[tile & 7];
    omH = gGfxRdp.otherModeH & OM_BIND_BITS;
    if (texMap == GX_TEXMAP0 || texMap == GX_TEXMAP1) {
        memo = &sMemo[texMap == GX_TEXMAP1];
        if (memo->valid && !memo->image && memo->frame == sFrame && memo->tmemGen == sTmemGen && memo->omH == omH &&
            memcmp(&memo->tile, t, sizeof(*t)) == 0) {
            sStats.hits++;
            sEntries[memo->entry].usedFrame = sFrame;
            /* Most recent again, so that binding the other texture map for the same draw never evicts this one */
            if (sLruHead != memo->entry) {
                lru_unlink(memo->entry);
                lru_push_front(memo->entry);
            }
            tex_load_obj(&memo->obj, texMap);
            *out = memo->binding;
            return true;
        }
        memo->valid = false;
    }

    conv = tex_conv(t->fmt, t->siz, omH);
    siz = sConvSiz[conv];
    is32 = siz == G_IM_SIZ_32b;
    mul = is32 ? 2 : 1;
    /* 32-bit texels live in the low TMEM half (blue/alpha in the high half); palette indices too, the high
     * half holding the TLUT: those addresses wrap at 2 KB */
    limit = (is32 || conv == CONV_CI4_RGBA || conv == CONV_CI4_IA || conv == CONV_CI8_RGBA || conv == CONV_CI8_IA)
                ? 2048
                : TMEM_BYTES;
    memset(&key, 0, sizeof(key));
    /* COPY mode only masks (and mirrors) texture coordinates: the RDP skips the clamp there */
    copy = (omH & (3u << G_MDSFT_CYCLETYPE)) == G_CYC_COPY;
    tex_axis(t->uls, t->lrs, t->masks, (copy && t->masks != 0) ? (t->cms & ~G_TX_CLAMP) : t->cms, &width, &srcW,
             &wrapS, &key.mapS);
    tex_axis(t->ult, t->lrt, t->maskt, (copy && t->maskt != 0) ? (t->cmt & ~G_TX_CLAMP) : t->cmt, &height, &srcH,
             &wrapT, &key.mapT);
    tmemAddr = (t->tmem * 8) & (limit - 1);
    lineBytes = t->line * 8;

    if ((uint32_t)width * height > TEX_MAX_TEXELS) {
        /* A tile size with lower right < upper left clamps at up to 1024 texels on the RDP. Keep clamped axes
         * within what TMEM holds: one line of texels, and the lines that fit in TMEM. */
        uint32_t lineTexels = (siz == G_IM_SIZ_4b) ? lineBytes * 2 : lineBytes / siz_tmem_bytes(siz);
        uint32_t tmemRows = lineBytes ? limit / lineBytes : 1;

        if (wrapS == GX_CLAMP && key.mapS == 0 && lineTexels != 0 && width > lineTexels) {
            width = srcW = lineTexels;
        }
        if (wrapT == GX_CLAMP && key.mapT == 0 && tmemRows != 0 && height > tmemRows) {
            height = srcH = tmemRows;
        }
        if ((uint32_t)width * height > TEX_MAX_TEXELS) {
            TEX_LOG_ONCE(LOG_TOO_BIG, "gfx: tex: %ux%u texture (tile %d) is too large", (unsigned int)width,
                         (unsigned int)height, tile & 7);
            return tex_fail(out);
        }
    }

    /* Texels: rows end on a RAM word (8 TMEM bytes, 4 for 32-bit texels) */
    ramRow = row_ram_bytes(siz, srcW);
    tmemRow = is32 ? ((ramRow / 2 + 3) & ~3u) : ((ramRow + 7) & ~7u);
    fast = tex_find_fast(tmemAddr, lineBytes, tmemRow, srcH, is32, limit, &src, &stride, &swapOdd);
    if (fast) {
        key.src = src;
        key.stride = stride;
        key.flags |= swapOdd ? KEY_SWAP_ODD : 0;
    } else {
        if (!tex_any_load(tmemAddr, (srcH - 1) * lineBytes + tmemRow, is32, limit)) {
            /* Nothing was loaded where the tile reads in this task (TMEM is not kept across tasks) */
            TEX_LOG_ONCE(LOG_UNLOADED, "gfx: tex: tile %d (fmt %d siz %d tmem %03X line %u, %ux%u) reads TMEM that no "
                         "load of this task wrote (%d loads)", tile & 7, t->fmt, t->siz, t->tmem, t->line,
                         (unsigned int)srcW, (unsigned int)srcH, sNumLoads);
            return tex_fail(out);
        }
        if (ramRow * srcH > sizeof(sScratch)) {
            TEX_LOG_ONCE(LOG_SCRATCH, "gfx: tex: %ux%u texture (tile %d) needs too much scratch", (unsigned int)srcW,
                         (unsigned int)srcH, tile & 7);
            return tex_fail(out);
        }
        vtmem_build();
        vtmem_gather(sScratch, tmemAddr >> 3, t->line, siz, srcW, srcH, limit);
        key.src = tex_hash(sScratch, ramRow * srcH, 0x27D4EB2F);
        key.flags |= KEY_SLOW;
        sStats.slow++;
    }

    /* Palette: CI4 uses entries palette * 16 .. + 15, CI8 all 256 */
    if (conv == CONV_CI4_RGBA || conv == CONV_CI4_IA) {
        palFirst = t->palette * 16;
        palCount = 16;
    } else if (conv == CONV_CI8_RGBA || conv == CONV_CI8_IA) {
        palCount = 256;
    }
    if (palCount != 0) {
        if (tex_find_pal(palFirst, palCount, &palAddr, &palRam) && tex_ram_ok(palAddr, palRam * 2)) {
            key.pal = palAddr;
            key.palCount = palRam;
            key.flags |= KEY_PAL_ADDR;
        } else {
            uint32_t i, w;

            vtmem_build();
            for (i = 0; i < palCount; i++) {
                w = ((256 + palFirst + i) * 8) & (TMEM_BYTES - 1);
                sPalScratch[i * 2] = sVTmem[w];
                sPalScratch[i * 2 + 1] = sVTmem[w + 1];
            }
            key.pal = tex_hash(sPalScratch, palCount * 2, 0x61C88647);
            key.flags |= KEY_PAL_HASH;
        }
    } else if (conv == CONV_CI4_I8) {
        key.pal = t->palette;
    }

    key.width = width;
    key.height = height;
    key.srcW = srcW;
    key.srcH = srcH;
    key.conv = conv;

    /* Cache lookup, with the source contents checked once per task */
    h = key_hash(&key);
    e = cache_find(&key, h);
    if (e >= 0) {
        en = &sEntries[e];
        if (en->validFrame != sFrame) {
            uint32_t dh = (key.flags & KEY_SLOW) ? 0 : tex_hash_ram(src, stride, tmemRow * mul, srcH);
            uint32_t ph = (key.flags & KEY_PAL_ADDR) ? tex_hash_ram(palAddr, palRam * 2, palRam * 2, 1) : 0;

            en->validFrame = sFrame;
            if (dh != en->dataHash || ph != en->palHash) {
                en->dataHash = dh;
                en->palHash = ph;
                convert = true;
                sStats.reconverts++;
            }
        }
        if (!convert) {
            sStats.hits++;
        }
        lru_unlink(e);
        lru_push_front(e);
    } else {
        size = gx_tex_bytes(sConvGxFmt[conv], width, height);
        if (fast && !tex_ram_ok(src, (srcH - 1) * stride + tmemRow * mul)) {
            return tex_fail(out);
        }
        e = cache_insert(&key, h, size);
        if (e < 0) {
            TEX_LOG_ONCE(LOG_NO_FIT, "gfx: tex: %ux%u texture (%u bytes) does not fit in the texture cache",
                         (unsigned int)width, (unsigned int)height, (unsigned int)size);
            return tex_fail(out);
        }
        en = &sEntries[e];
        en->validFrame = sFrame;
        en->dataHash = (key.flags & KEY_SLOW) ? 0 : tex_hash_ram(src, stride, tmemRow * mul, srcH);
        en->palHash = (key.flags & KEY_PAL_ADDR) ? tex_hash_ram(palAddr, palRam * 2, palRam * 2, 1) : 0;
        convert = true;
        sStats.misses++;
    }
    if (convert && en->usedFrame == sFrame) {
        tex_sync_rewrite();
    }
    en->usedFrame = sFrame;

    if (convert) {
        TexJob job;

        if (key.flags & KEY_PAL_ADDR) {
            tex_convert_pal(gfx_addr(palAddr), palRam, palCount, conv);
        } else if (palCount != 0) {
            tex_convert_pal(sPalScratch, palCount, palCount, conv);
        }
        if (fast && !swapOdd) {
            job.rows = gfx_addr(src);
            job.rowStride = stride;
        } else {
            if (fast) {
                vtmem_build();
                vtmem_gather(sScratch, tmemAddr >> 3, t->line, siz, srcW, srcH, limit);
            }
            job.rows = sScratch;
            job.rowStride = ramRow;
        }
        tex_build_map(sMapS, width, key.mapS);
        tex_build_map(sMapT, height, key.mapT);
        job.conv = conv;
        job.width = width;
        job.height = height;
        job.ci4pal = t->palette;
        job.dst = sArena + en->offset;
        tex_run(&job);
        DCFlushRange(job.dst, gx_tex_bytes(sConvGxFmt[conv], width, height));
        sNeedInvalidate = true;
    }

    /* GX texture object and the binding */
    linear = (gGfxRdp.otherModeH & (3u << G_MDSFT_TEXTFILT)) != G_TF_POINT &&
             (gGfxRdp.otherModeH & (3u << G_MDSFT_CYCLETYPE)) != G_CYC_COPY;
    GX_InitTexObj(&obj, sArena + en->offset, width, height, sConvGxFmt[conv], wrapS, wrapT, GX_FALSE);
    GX_InitTexObjFilterMode(&obj, linear ? GX_LINEAR : GX_NEAR, linear ? GX_LINEAR : GX_NEAR);
    tex_load_obj(&obj, texMap);

    out->valid = true;
    out->width = width;
    out->height = height;
    out->sShiftScale = shift_scale(t->shifts);
    out->tShiftScale = shift_scale(t->shiftt);
    /* The RDP's bilinear filter samples texel s at s, GX at s + 0.5: shift by half a texel (as Fast3D does) */
    out->sOffset = t->uls * 0.25f - (linear ? 0.5f : 0.0f);
    out->tOffset = t->ult * 0.25f - (linear ? 0.5f : 0.0f);
    out->linear = linear;

    if (memo != NULL) {
        memo->valid = true;
        memo->image = false;
        memo->entry = e;
        memo->frame = sFrame;
        memo->tmemGen = sTmemGen;
        memo->omH = omH;
        memo->tile = *t;
        memo->obj = obj;
        memo->binding = *out;
    }
    return true;
}

/* Whether `bytes` at a CPU pointer are game RAM (MEM1, cached or uncached mirror). Host tests use host pointers. */
static bool tex_ptr_ok(const void* p, uint32_t bytes) {
#ifdef GEKKO
    uint32_t a = (uint32_t)p;

    if ((a >> 30) >= 2 && (a & 0x3FFFFFFF) < TEX_RAM_END - TEX_RAM_START &&
        bytes <= TEX_RAM_END - TEX_RAM_START - (a & 0x3FFFFFFF)) {
        return true;
    }
    TEX_LOG_ONCE(LOG_RAM, "gfx: tex: image address %08X (+%u) is outside RAM", (unsigned int)a, (unsigned int)bytes);
    return false;
#else
    return p != NULL;
#endif
}

bool gfx_tex_bind_image(const void* addr, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height, uint16_t stride,
                        const void* tlut, bool tlutIA, bool linear, int texMap, GfxTexBinding* out) {
    const uint8_t* rows = addr;
    TexMemo* memo = NULL;
    TexKey key;
    TexEntry* en;
    GXTexObj obj;
    uint32_t rowBytes, strideBytes, palCount = 0, h;
    int conv, gxFmt, e;
    bool convert = false;

    sStats.binds++;
    out->valid = false;
    if (!sInit) {
        gfx_tex_init();
    }
    if (texMap == GX_TEXMAP0 || texMap == GX_TEXMAP1) {
        /* The map is about to hold this image, not the memo's tile */
        memo = &sMemo[texMap == GX_TEXMAP1];
        memo->valid = false;
    }
    if (sArena == NULL) {
        return tex_fail(out);
    }
    if (rows == NULL || width == 0 || height == 0 || width > TEX_MAX_DIM || height > TEX_MAX_DIM || stride < width) {
        TEX_LOG_ONCE(LOG_IMAGE, "gfx: tex: image %ux%u (stride %u) at %08X not supported", (unsigned int)width,
                     (unsigned int)height, (unsigned int)stride, (unsigned int)(uintptr_t)addr);
        return tex_fail(out);
    }

    /* Formats as the RDP decodes them; a palette applies to 4- and 8-bit texels */
    if (fmt == G_IM_FMT_YUV && siz == G_IM_SIZ_16b) {
        conv = CONV_YUV16;
    } else {
        conv = tex_conv(fmt, siz, (tlut != NULL) ? (OM_TEXTLUT_ON | (tlutIA ? OM_TEXTLUT_IA : 0)) : 0);
    }
    gxFmt = sConvGxFmt[conv];
    rowBytes = row_ram_bytes(sConvSiz[conv], (conv == CONV_YUV16) ? (width + 1u) & ~1u : width);
    strideBytes = row_ram_bytes(sConvSiz[conv], stride);
    if (conv == CONV_CI4_RGBA || conv == CONV_CI4_IA) {
        palCount = 16;
    } else if (conv == CONV_CI8_RGBA || conv == CONV_CI8_IA) {
        palCount = 256;
    }
    if (!tex_ptr_ok(rows, (height - 1u) * strideBytes + rowBytes) ||
        (palCount != 0 && !tex_ptr_ok(tlut, palCount * 2))) {
        return tex_fail(out);
    }

    memset(&key, 0, sizeof(key));
    key.src = tex_key_addr((uint32_t)(uintptr_t)rows);
    key.stride = strideBytes;
    key.width = key.srcW = width;
    key.height = key.srcH = height;
    key.conv = conv;
    key.flags = KEY_IMAGE;
    if (palCount != 0) {
        key.pal = tex_key_addr((uint32_t)(uintptr_t)tlut);
        key.palCount = palCount;
        key.flags |= KEY_PAL_ADDR;
    }

    /* Cache lookup, with the contents checked once per task */
    h = key_hash(&key);
    e = cache_find(&key, h);
    if (e >= 0) {
        en = &sEntries[e];
        if (en->validFrame != sFrame) {
            uint32_t dh = tex_hash_rows(rows, strideBytes, rowBytes, height);
            uint32_t ph = (palCount != 0) ? tex_hash_rows(tlut, palCount * 2, palCount * 2, 1) : 0;

            en->validFrame = sFrame;
            if (dh != en->dataHash || ph != en->palHash) {
                en->dataHash = dh;
                en->palHash = ph;
                convert = true;
                sStats.reconverts++;
            }
        }
        if (!convert) {
            sStats.hits++;
        }
        lru_unlink(e);
        lru_push_front(e);
    } else {
        e = cache_insert(&key, h, gx_tex_bytes(gxFmt, width, height));
        if (e < 0) {
            TEX_LOG_ONCE(LOG_NO_FIT, "gfx: tex: %ux%u image (%u bytes) does not fit in the texture cache",
                         (unsigned int)width, (unsigned int)height, (unsigned int)gx_tex_bytes(gxFmt, width, height));
            return tex_fail(out);
        }
        en = &sEntries[e];
        en->validFrame = sFrame;
        en->dataHash = tex_hash_rows(rows, strideBytes, rowBytes, height);
        en->palHash = (palCount != 0) ? tex_hash_rows(tlut, palCount * 2, palCount * 2, 1) : 0;
        convert = true;
        sStats.misses++;
    }
    if (convert && en->usedFrame == sFrame) {
        tex_sync_rewrite();
    }
    en->usedFrame = sFrame;

    if (convert) {
        TexJob job;

        if (palCount != 0) {
            tex_convert_pal(tlut, palCount, palCount, conv);
        }
        tex_build_map(sMapS, width, 0);
        tex_build_map(sMapT, height, 0);
        job.conv = conv;
        job.width = width;
        job.height = height;
        job.rows = rows;
        job.rowStride = strideBytes;
        job.ci4pal = 0;
        job.dst = sArena + en->offset;
        tex_run(&job);
        DCFlushRange(job.dst, gx_tex_bytes(gxFmt, width, height));
        sNeedInvalidate = true;
    }

    GX_InitTexObj(&obj, sArena + en->offset, width, height, gxFmt, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&obj, linear ? GX_LINEAR : GX_NEAR, linear ? GX_LINEAR : GX_NEAR);
    tex_load_obj(&obj, texMap);

    out->valid = true;
    out->width = width;
    out->height = height;
    out->sOffset = out->tOffset = 0.0f;
    out->sShiftScale = out->tShiftScale = 1.0f;
    out->linear = linear;

    if (memo != NULL) {
        memo->valid = true;
        memo->image = true;
        memo->entry = e;
        memo->frame = sFrame;
    }
    return true;
}

/* ================================================================================================ */
/* Lifecycle                                                                                        */
/* ================================================================================================ */

void gfx_tex_init(void) {
    uint32_t size;
    int i;

    if (sInit) {
        return;
    }
    sInit = true;
    for (size = TEX_CACHE_SIZE; size >= TEX_CACHE_MIN; size >>= 1) {
        sArena = gc_mem_alloc(size, 32);
        if (sArena != NULL) {
            break;
        }
        gc_log("gfx: tex: no memory for a %u KB texture cache", (unsigned int)(size >> 10));
    }
    if (sArena == NULL) {
        gc_log("gfx: tex: no texture cache: textures disabled");
        return;
    }
    sArenaSize = size;
    for (sMaxOrder = 0; (1u << sMaxOrder) < size; sMaxOrder++) {}
    for (i = 0; i < 32; i++) {
        sFreeHead[i] = TEX_NONE;
    }
    memset(sFreeBits, 0, sizeof(sFreeBits));
    buddy_push(sMaxOrder, 0);

    for (i = 0; i < TEX_HASH_SIZE; i++) {
        sHash[i] = -1;
    }
    for (i = 0; i < TEX_MAX_ENTRIES; i++) {
        sEntries[i].used = 0;
        sEntries[i].hashNext = (i + 1 < TEX_MAX_ENTRIES) ? i + 1 : -1;
    }
    sFreeEntry = 0;
    sLruHead = sLruTail = -1;
    memset(&sStats, 0, sizeof(sStats));
    sStats.bytesTotal = size;
    gfx_tex_reset();
    gc_log("gfx: tex: %u KB texture cache", (unsigned int)(size >> 10));
}

void gfx_tex_reset(void) {
    sNumLoads = 0;
    sTmemGen++;
    sMemo[0].valid = sMemo[1].valid = false;
}

void gfx_tex_frame(void) {
    /* Contents are checked lazily: the first bind of each entry in a task hashes its source again */
    sFrame++;
    sMemo[0].valid = sMemo[1].valid = false;
}

void gfx_tex_ram_written(uint32_t addr, uint32_t bytes) {
    uint32_t a = tex_key_addr(addr), b = a + bytes;
    int e;

    /* The replayed TMEM is rebuilt from RAM; textures read from the range are hashed again at their next bind */
    sVTmemGen = 0;
    for (e = sLruHead; e >= 0; e = sEntries[e].lruNext) {
        TexEntry* en = &sEntries[e];
        const TexKey* k = &en->key;
        bool hit = false;

        if (!(k->flags & KEY_SLOW)) {
            uint32_t rowBytes = row_ram_bytes(sConvSiz[k->conv], k->srcW) * 2 + 8; /* bound: 32-bit tiles, padding */

            hit = range_overlap(k->src, k->src + (k->srcH - 1u) * k->stride + rowBytes, a, b);
        }
        if (k->flags & KEY_PAL_ADDR) {
            hit |= range_overlap(k->pal, k->pal + k->palCount * 2u, a, b);
        }
        if (hit) {
            en->validFrame = 0;
            if (sMemo[0].entry == e) {
                sMemo[0].valid = false;
            }
            if (sMemo[1].entry == e) {
                sMemo[1].valid = false;
            }
        }
    }
}

void gfx_tex_get_stats(GfxTexStats* out) {
    *out = sStats;
}

/**
 * RDP side of the N64 renderer: the RDP commands forwarded by the display list interpreter (color, z and
 * texture images, combiner, other modes, colors, tiles, TMEM loads, scissor) update gGfxRdp, and fill and
 * texture rectangles are decoded into N64 pixel coordinates for gfx_gx.c. TMEM loads are recorded by
 * gfx_tex.c. Texture rectangles whose texture repeats within an N64 pixel are drawn in one-pixel strips, so that
 * the higher-resolution EFB samples them as the RDP does.
 */
#include <math.h>
#include <string.h>
#include "gfx_internal.h"

/* Texture rectangles drawn pixel by pixel (strips along both axes) at most this large; larger ones get rows only */
#define RDP_RECT_MAX_CELLS 4096

GfxRdpState gGfxRdp;

/* G_SETCONVERT / G_SETKEYR / G_SETKEYGB. Kept for completeness: nothing on GX uses YUV conversion or the
 * chroma key (MM never decodes its JPEG code's YUV output, and never keys). */
static struct {
    int16_t k[6];
    uint32_t keyR;
    uint32_t keyGB[2];
} sRdpMisc;

/* Other mode H fields that change how a tile is sampled (gfx_tex_bind reads them) */
#define OTHERMODE_H_TEX_BITS \
    ((3u << G_MDSFT_CYCLETYPE) | (1u << G_MDSFT_TEXTLOD) | (3u << G_MDSFT_TEXTLUT) | (3u << G_MDSFT_TEXTFILT))

static uint8_t sUnknownLogged[256 / 8];

static void rdp_log_unknown(uint32_t w0, uint32_t w1) {
    uint32_t op = w0 >> 24;

    if (!(sUnknownLogged[op >> 3] & (1 << (op & 7)))) {
        sUnknownLogged[op >> 3] |= 1 << (op & 7);
        gc_log("gfx: rdp: unhandled command %08X %08X", (unsigned int)w0, (unsigned int)w1);
    }
}

/* F3DEX2 resolves segmented addresses when it forwards G_SETTIMG, so later segment changes do not move an
 * image that was already set. Same mapping as gfx_addr(), kept as a 32-bit address. */
static uint32_t rdp_resolve(uint32_t addr) {
    if (addr == 0) {
        return 0;
    }
    if (addr & 0x80000000) {
        return 0x80000000 | (addr & 0x1FFFFFFF);
    }
    return 0x80000000 | (gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF));
}

void gfx_rdp_reset(void) {
    memset(&gGfxRdp, 0, sizeof(gGfxRdp));
    /* Neutral start (perspective textures, filtered conversion, no dither); MM sets its own modes every frame */
    gGfxRdp.otherModeH = 0x00080CFF;
    gGfxRdp.scissorLrx = GFX_N64_WIDTH << 2;
    gGfxRdp.scissorLry = GFX_N64_HEIGHT << 2;
    gGfxRdp.dirty = GFX_DIRTY_ALL;
    gfx_tex_reset();
}

static void rdp_set_othermode(uint32_t newH, uint32_t newL) {
    if (newH != gGfxRdp.otherModeH || newL != gGfxRdp.otherModeL) {
        gGfxRdp.dirty |= GFX_DIRTY_OTHERMODE;
        if ((newH ^ gGfxRdp.otherModeH) & OTHERMODE_H_TEX_BITS) {
            gGfxRdp.dirty |= GFX_DIRTY_TEXTURES;
        }
        gGfxRdp.otherModeH = newH;
        gGfxRdp.otherModeL = newL;
    }
}

/* G_SETOTHERMODE_H/L, F3DEX2 encoding (gSPSetOtherMode): w0 bits 8..15 hold 32 - shift - len, bits 0..7 hold
 * len - 1. Like the microcode, the data word is ORed in without masking. */
static uint32_t rdp_othermode_word(uint32_t word, uint32_t w0, uint32_t w1) {
    int len = (int)(w0 & 0xFF) + 1;
    int sft = 32 - (int)((w0 >> 8) & 0xFF) - len;
    uint32_t mask;

    if (sft < 0) {
        rdp_log_unknown(w0, w1);
        return word;
    }
    mask = (len >= 32) ? 0xFFFFFFFF : (((1u << len) - 1) << sft);
    return (word & ~mask) | w1;
}

static void rdp_set_color(uint32_t* color, uint32_t value) {
    if (*color != value) {
        *color = value;
        gGfxRdp.dirty |= GFX_DIRTY_COLORS;
    }
}

static void rdp_set_tile_size(int tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    GfxTile* t = &gGfxRdp.tiles[tile];

    t->uls = uls;
    t->ult = ult;
    t->lrs = lrs;
    t->lrt = lrt;
    gGfxRdp.dirty |= GFX_DIRTY_TEXTURES;
}

static bool rdp_copy_or_fill(void) {
    uint32_t cycle = gGfxRdp.otherModeH & (3u << G_MDSFT_CYCLETYPE);

    return cycle == G_CYC_COPY || cycle == G_CYC_FILL;
}

/* G_FILLRECT: lower right in w0, upper left in w1, 10.2 fixed point. In FILL and COPY mode the RDP works on
 * whole pixels and includes the lower right edge; in 1/2-cycle mode the edge is exclusive. */
static void rdp_fill_rect(uint32_t w0, uint32_t w1) {
    uint32_t lrx = (w0 >> 12) & 0xFFF;
    uint32_t lry = w0 & 0xFFF;
    uint32_t ulx = (w1 >> 12) & 0xFFF;
    uint32_t uly = w1 & 0xFFF;
    float x0, y0, x1, y1;

    if (rdp_copy_or_fill()) {
        x0 = (float)(ulx >> 2);
        y0 = (float)(uly >> 2);
        x1 = (float)((lrx >> 2) + 1);
        y1 = (float)((lry >> 2) + 1);
    } else {
        x0 = ulx * 0.25f;
        y0 = uly * 0.25f;
        x1 = lrx * 0.25f;
        y1 = lry * 0.25f;
    }
    if (x1 > x0 && y1 > y0) {
        gfx_gx_fillrect(x0, y0, x1, y1);
    }
}

void gfx_rdp_command(uint32_t w0, uint32_t w1) {
    uint32_t op = w0 >> 24;
    int tile = (w1 >> 24) & 7;

    switch (op) {
        /* Image widths: gbi.h packs 12 bits, the RDP reads 10 */
        case G_SETCIMG: {
            uint8_t fmt = (w0 >> 21) & 7;
            uint8_t siz = (w0 >> 19) & 3;
            uint16_t width = (w0 & 0x3FF) + 1;

            if (w1 != gGfxRdp.colorImageAddr || fmt != gGfxRdp.colorImageFmt || siz != gGfxRdp.colorImageSiz ||
                width != gGfxRdp.colorImageWidth) {
                gfx_gx_flush();
                gGfxRdp.colorImageAddr = w1;
                gGfxRdp.colorImageFmt = fmt;
                gGfxRdp.colorImageSiz = siz;
                gGfxRdp.colorImageWidth = width;
            }
            break;
        }
        case G_SETZIMG:
            gGfxRdp.zImageAddr = w1;
            break;
        case G_SETTIMG:
            gGfxRdp.texImageAddr = rdp_resolve(w1);
            gGfxRdp.texImageFmt = (w0 >> 21) & 7;
            gGfxRdp.texImageSiz = (w0 >> 19) & 3;
            gGfxRdp.texImageWidth = (w0 & 0x3FF) + 1;
            break;

        case G_SETCOMBINE:
            if ((w0 & 0x00FFFFFF) != gGfxRdp.combineHi || w1 != gGfxRdp.combineLo) {
                gGfxRdp.combineHi = w0 & 0x00FFFFFF;
                gGfxRdp.combineLo = w1;
                gGfxRdp.dirty |= GFX_DIRTY_COMBINE;
            }
            break;
        case G_SETENVCOLOR:
            rdp_set_color(&gGfxRdp.envColor, w1);
            break;
        case G_SETPRIMCOLOR:
            rdp_set_color(&gGfxRdp.primColor, w1);
            if (gGfxRdp.primLodMin != ((w0 >> 8) & 0xFF) || gGfxRdp.primLodFrac != (w0 & 0xFF)) {
                gGfxRdp.primLodMin = (w0 >> 8) & 0xFF;
                gGfxRdp.primLodFrac = w0 & 0xFF;
                gGfxRdp.dirty |= GFX_DIRTY_COLORS;
            }
            break;
        case G_SETBLENDCOLOR:
            rdp_set_color(&gGfxRdp.blendColor, w1);
            break;
        case G_SETFOGCOLOR:
            rdp_set_color(&gGfxRdp.fogColor, w1);
            break;
        case G_SETFILLCOLOR:
            rdp_set_color(&gGfxRdp.fillColor, w1);
            break;
        case G_SETPRIMDEPTH:
            /* Used by z source G_ZS_PRIM: a z-buffer state, so it travels with the other mode */
            gGfxRdp.primDepthZ = w1 >> 16;
            gGfxRdp.primDepthDZ = w1 & 0xFFFF;
            gGfxRdp.dirty |= GFX_DIRTY_OTHERMODE;
            break;

        case G_RDPSETOTHERMODE:
            rdp_set_othermode(w0 & 0x00FFFFFF, w1);
            break;
        case G_SETOTHERMODE_H:
            rdp_set_othermode(rdp_othermode_word(gGfxRdp.otherModeH, w0, w1), gGfxRdp.otherModeL);
            break;
        case G_SETOTHERMODE_L:
            rdp_set_othermode(gGfxRdp.otherModeH, rdp_othermode_word(gGfxRdp.otherModeL, w0, w1));
            break;

        case G_SETTILE: {
            GfxTile* t = &gGfxRdp.tiles[tile];

            t->fmt = (w0 >> 21) & 7;
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 0x1FF;
            t->tmem = w0 & 0x1FF;
            t->palette = (w1 >> 20) & 0xF;
            t->cmt = (w1 >> 18) & 3;
            t->maskt = (w1 >> 14) & 0xF;
            t->shiftt = (w1 >> 10) & 0xF;
            t->cms = (w1 >> 8) & 3;
            t->masks = (w1 >> 4) & 0xF;
            t->shifts = w1 & 0xF;
            gGfxRdp.dirty |= GFX_DIRTY_TEXTURES;
            break;
        }
        case G_SETTILESIZE:
            rdp_set_tile_size(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        /* The loads also overwrite the load tile's size registers, as on the RDP (LOADBLOCK stores dxt as lrt) */
        case G_LOADBLOCK:
            rdp_set_tile_size(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            gfx_tex_load_block(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTILE:
            rdp_set_tile_size(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            gfx_tex_load_tile(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;
        case G_LOADTLUT:
            rdp_set_tile_size(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            gfx_tex_load_tlut(tile, (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF);
            break;

        case G_SETSCISSOR: {
            uint16_t ulx = (w0 >> 12) & 0xFFF, uly = w0 & 0xFFF;
            uint16_t lrx = (w1 >> 12) & 0xFFF, lry = w1 & 0xFFF;
            uint8_t mode = (w1 >> 24) & 3;

            if (ulx != gGfxRdp.scissorUlx || uly != gGfxRdp.scissorUly || lrx != gGfxRdp.scissorLrx ||
                lry != gGfxRdp.scissorLry || mode != gGfxRdp.scissorMode) {
                gGfxRdp.scissorUlx = ulx;
                gGfxRdp.scissorUly = uly;
                gGfxRdp.scissorLrx = lrx;
                gGfxRdp.scissorLry = lry;
                gGfxRdp.scissorMode = mode;
                gGfxRdp.dirty |= GFX_DIRTY_SCISSOR;
            }
            break;
        }
        case G_FILLRECT:
            rdp_fill_rect(w0, w1);
            break;

        case G_SETCONVERT: {
            uint32_t k2 = ((w0 & 0xF) << 5) | (w1 >> 27);

            /* 9-bit signed coefficients */
            sRdpMisc.k[0] = (int16_t)(((w0 >> 13) & 0x1FF) << 7) >> 7;
            sRdpMisc.k[1] = (int16_t)(((w0 >> 4) & 0x1FF) << 7) >> 7;
            sRdpMisc.k[2] = (int16_t)(k2 << 7) >> 7;
            sRdpMisc.k[3] = (int16_t)(((w1 >> 18) & 0x1FF) << 7) >> 7;
            sRdpMisc.k[4] = (w1 >> 9) & 0x1FF;
            sRdpMisc.k[5] = w1 & 0x1FF;
            break;
        }
        case G_SETKEYR:
            sRdpMisc.keyR = w1;
            break;
        case G_SETKEYGB:
            sRdpMisc.keyGB[0] = w0 & 0x00FFFFFF;
            sRdpMisc.keyGB[1] = w1;
            break;

        case G_RDPFULLSYNC:
        case G_RDPPIPESYNC:
        case G_RDPTILESYNC:
        case G_RDPLOADSYNC:
        case G_NOOP:
            break;

        default:
            rdp_log_unknown(w0, w1);
            break;
    }
}

/* Whether a tile axis wraps (mask without clamp) at a rate of at least half its period per N64 pixel, after the
 * tile shift. The RDP samples a texture rectangle once per pixel, at the pixel's own s/t; the EFB samples each N64
 * pixel GFX_SCALE times along the axis, and those samples would land on unrelated texels of the pattern. The N64
 * logo's shine (shift 11, mask 5) steps 32 texels per row: every row of a rectangle reads the same shine row, where
 * the EFB's two rows per N64 row would read rows 16 texels apart. */
static bool rdp_rect_axis_aliases(const GfxTile* t, bool sAxis, float step) {
    uint32_t mask = sAxis ? t->masks : t->maskt;
    uint32_t cm = sAxis ? t->cms : t->cmt;
    uint32_t shift = sAxis ? t->shifts : t->shiftt;
    float period, scaled;

    if (mask == 0 || (cm & G_TX_CLAMP)) {
        return false;
    }
    period = (float)(1u << ((mask > 10) ? 10 : mask)) * ((cm & G_TX_MIRROR) ? 2.0f : 1.0f);
    scaled = (step < 0.0f) ? -step : step;
    if (shift > 10) {
        scaled *= (float)(1u << (16 - shift));
    } else {
        scaled /= (float)(1u << shift);
    }
    return scaled >= period * 0.5f;
}

/* Draw a texture rectangle as strips one N64 pixel wide along x and/or y, with s/t constant across each strip
 * along that axis, as the RDP computes them per pixel */
static void rdp_texrect_strips(float x0, float y0, float x1, float y1, int tile, float s, float t, float dsdx,
                               float dtdy, bool flip, bool rows, bool cols) {
    /* Per screen axis: which texture coordinate advances along it, and by how much per pixel */
    float stepX = flip ? dtdy : dsdx, stepY = flip ? dsdx : dtdy;
    float ry0, ry1, cx0, cx1;

    for (ry0 = y0; ry0 < y1; ry0 = ry1) {
        float rs = s, rt = t, rStepY = stepY;

        ry1 = rows ? floorf(ry0) + 1.0f : y1;
        if (ry1 > y1) {
            ry1 = y1;
        }
        if (rows) {
            /* Constant along y in this strip */
            if (flip) {
                rs = s + (ry0 - y0) * dsdx;
            } else {
                rt = t + (ry0 - y0) * dtdy;
            }
            rStepY = 0.0f;
        }
        for (cx0 = x0; cx0 < x1; cx0 = cx1) {
            float cs = rs, ct = rt, cStepX = stepX;

            cx1 = cols ? floorf(cx0) + 1.0f : x1;
            if (cx1 > x1) {
                cx1 = x1;
            }
            if (cols) {
                if (flip) {
                    ct = rt + (cx0 - x0) * dtdy;
                } else {
                    cs = rs + (cx0 - x0) * dsdx;
                }
                cStepX = 0.0f;
            }
            if (flip) {
                gfx_gx_texrect(cx0, ry0, cx1, ry1, tile, cs, ct, rStepY, cStepX, true);
            } else {
                gfx_gx_texrect(cx0, ry0, cx1, ry1, tile, cs, ct, cStepX, rStepY, false);
            }
        }
    }
}

/* G_TEXRECT / G_TEXRECTFLIP: w0 = lower right (10.2), w1 = tile and upper left (10.2), half1 = s, t at the
 * upper left (s10.5), half2 = dsdx, dtdy (s5.10). In COPY mode the RDP copies four texels per clock, so the
 * game passes dsdx = 4.0 for one texel per pixel, and the lower right edge is inclusive. */
void gfx_rdp_texrect(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip) {
    uint32_t lrx = (w0 >> 12) & 0xFFF;
    uint32_t lry = w0 & 0xFFF;
    int tile = (w1 >> 24) & 7;
    uint32_t ulx = (w1 >> 12) & 0xFFF;
    uint32_t uly = w1 & 0xFFF;
    float s = (int16_t)(half1 >> 16) * (1.0f / 32.0f);
    float t = (int16_t)(half1 & 0xFFFF) * (1.0f / 32.0f);
    float dsdx = (int16_t)(half2 >> 16) * (1.0f / 1024.0f);
    float dtdy = (int16_t)(half2 & 0xFFFF) * (1.0f / 1024.0f);
    uint32_t cycle = gGfxRdp.otherModeH & (3u << G_MDSFT_CYCLETYPE);
    float x0, y0, x1, y1;
    bool rows = false, cols = false;
    int i;

    if (rdp_copy_or_fill()) {
        if (cycle == G_CYC_COPY) {
            dsdx *= 0.25f;
        }
        x0 = (float)(ulx >> 2);
        y0 = (float)(uly >> 2);
        x1 = (float)((lrx >> 2) + 1);
        y1 = (float)((lry >> 2) + 1);
    } else {
        x0 = ulx * 0.25f;
        y0 = uly * 0.25f;
        x1 = lrx * 0.25f;
        y1 = lry * 0.25f;

        /* The tiles the combiner can read: TEXEL0, and TEXEL1 (tile + 1) in 2-cycle mode */
        for (i = 0; i < ((cycle == G_CYC_2CYCLE) ? 2 : 1); i++) {
            const GfxTile* tl = &gGfxRdp.tiles[(tile + i) & 7];

            rows |= rdp_rect_axis_aliases(tl, flip, flip ? dsdx : dtdy);
            cols |= rdp_rect_axis_aliases(tl, !flip, flip ? dtdy : dsdx);
        }
        if (rows && cols && (x1 - x0) * (y1 - y0) > RDP_RECT_MAX_CELLS) {
            cols = false; /* not expected in MM: rows only, bounded */
        }
    }
    if (x1 > x0 && y1 > y0) {
        if (rows || cols) {
            rdp_texrect_strips(x0, y0, x1, y1, tile, s, t, dsdx, dtdy, flip, rows, cols);
        } else {
            gfx_gx_texrect(x0, y0, x1, y1, tile, s, t, dsdx, dtdy, flip);
        }
    }
}

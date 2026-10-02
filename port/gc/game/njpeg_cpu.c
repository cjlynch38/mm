/**
 * The RSP JPEG task (M_NJPEGTASK, z_jpeg.c's njpgdspMain microcode) on the CPU, run by osSpTaskStartGo (sp.c).
 *
 * z_jpeg.c decodes the Huffman stream on the CPU (JpegDecoder_Decode) into macroblocks of quantized DCT
 * coefficients in zigzag order (JpegWork.data, one block of 64 after the other), then runs the task over mbCount
 * of them. The task turns each macroblock into RGBA5551 pixels written over the macroblock's own buffer, 16 pixels
 * per row, which Jpeg_CopyToZbuffer then copies out:
 *   - mode 2 (Y sampled 2x2): blocks Y0 Y1 Y2 Y3 U V, 16x16 pixels, chroma at half resolution in both directions
 *   - mode 0 (Y sampled 2x1): blocks Y0 Y1 U V, 16x8 pixels, chroma at half resolution horizontally
 * Each block: coefficient * quantization table entry (Y table for Y, U and V tables for the chroma blocks),
 * saturated to s16 and scaled by 16, out of zigzag order, 8x8 inverse DCT; each pixel: YUV -> RGB with the JFIF
 * coefficients, 5 bits per channel, alpha 1. This follows the HLE description of the microcode, which Ocarina of
 * Time shares (mupen64plus-rsp-hle, jpeg_decode_PS). In MM nothing calls Jpeg_Decode: its rooms with
 * pre-rendered backgrounds are never drawn with one (z_room.c), so the task only has to work if a caller appears.
 */
#include "ultra64.h"
#include "stdbool.h"
#include "z64jpeg.h"
#include "gc_game.h"

#define NJPEG_BLOCK 64
#define NJPEG_MAX_BLOCKS 6

/* Raster position of each coefficient in zigzag order */
static const u8 sNJpegZigzag[NJPEG_BLOCK] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
    41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
    30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* cos(k * pi / 16), k = 0..8 */
static const f32 sNJpegCos[9] = {
    1.0f,        0.98078528f, 0.92387953f, 0.83146961f, 0.70710678f,
    0.55557023f, 0.38268343f, 0.19509032f, 0.0f,
};

/* 1D inverse DCT basis: sNJpegBasis[u][x] = c(u) / 2 * cos((2x + 1) u pi / 16), c(0) = 1 / sqrt(2), c(u) = 1 */
static f32 sNJpegBasis[8][8];
static s32 sNJpegBasisReady;

static void NJpeg_InitBasis(void) {
    s32 u;
    s32 x;

    for (u = 0; u < 8; u++) {
        for (x = 0; x < 8; x++) {
            s32 k = ((2 * x + 1) * u) % 32;
            f32 c;

            if (k > 16) {
                k = 32 - k;
            }
            c = (k > 8) ? -sNJpegCos[16 - k] : sNJpegCos[k];
            sNJpegBasis[u][x] = (u == 0) ? 0.35355339f : 0.5f * c;
        }
    }
    sNJpegBasisReady = true;
}

/**
 * Dequantize one block (coefficients in zigzag order, like the table) and replace it with its inverse DCT, in
 * 1/16 units of the pixel value minus 128. The result is truncated as the microcode's: (s16)(8 * v) >> 3.
 */
static void NJpeg_DecodeBlock(s16* blk, const u16* qt) {
    f32 in[NJPEG_BLOCK];
    f32 rows[NJPEG_BLOCK];
    s32 i;
    s32 x;
    s32 y;
    s32 u;

    for (i = 0; i < NJPEG_BLOCK; i++) {
        s32 v = blk[i] * (s16)qt[i];

        v = (v < -0x8000) ? -0x8000 : (v > 0x7FFF) ? 0x7FFF : v;
        in[sNJpegZigzag[i]] = (s16)(v * 16);
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            f32 sum = 0.0f;

            for (u = 0; u < 8; u++) {
                sum += sNJpegBasis[u][x] * in[y * 8 + u];
            }
            rows[y * 8 + x] = sum;
        }
    }
    for (x = 0; x < 8; x++) {
        for (y = 0; y < 8; y++) {
            f32 sum = 0.0f;

            for (u = 0; u < 8; u++) {
                sum += sNJpegBasis[u][y] * rows[u * 8 + x];
            }
            sum *= 8.0f;
            sum = (sum < -32768.0f) ? -32768.0f : (sum > 32767.0f) ? 32767.0f : sum;
            blk[y * 8 + x] = (s16)((s16)sum >> 3);
        }
    }
}

/* One RGBA5551 channel from 12-bit fixed point (16 * value), clamped to [0, 0xFF0] */
static u32 NJpeg_Channel(f32 c) {
    if (!(c > 0.0f)) {
        return 0;
    }
    if (c > 4080.0f) {
        c = 4080.0f;
    }
    return (u32)(s32)c & 0xF80;
}

/* Y (centered on 0), U, V in 1/16 units -> RGBA5551 */
static u16 NJpeg_Pixel(s32 y, s32 u, s32 v) {
    f32 fy = y + 2048.0f;
    u32 r = NJpeg_Channel(fy + 1.4025f * v);
    u32 g = NJpeg_Channel(fy - 0.3443f * u - 0.7144f * v);
    u32 b = NJpeg_Channel(fy + 1.7729f * u);

    return (r << 4) | (g >> 1) | (b >> 6) | 1;
}

void Gc_NJpegRunTask(OSTask* task) {
    JpegTaskData* data = (JpegTaskData*)task->t.data_ptr;
    s16 mb[NJPEG_MAX_BLOCKS * NJPEG_BLOCK];
    u16 qt[3][NJPEG_BLOCK];
    const u16* tables[3];
    s16* buf;
    u32 mode;
    u32 n;
    s32 blocks;
    s32 i;
    s32 x;
    s32 y;

    if (data == NULL || data->address == NULL) {
        return;
    }
    mode = data->mode;
    if ((mode != 0) && (mode != 2)) {
        return;
    }
    if (!sNJpegBasisReady) {
        NJpeg_InitBasis();
    }
    blocks = mode + 4;
    tables[0] = data->qTableYPtr;
    tables[1] = data->qTableUPtr;
    tables[2] = data->qTableVPtr;
    for (i = 0; i < 3; i++) {
        for (x = 0; x < NJPEG_BLOCK; x++) {
            qt[i][x] = (tables[i] != NULL) ? tables[i][x] : 1;
        }
    }

    buf = data->address;
    for (n = 0; n < data->mbCount; n++, buf += blocks * NJPEG_BLOCK) {
        u16* out = (u16*)buf;

        for (i = 0; i < blocks * NJPEG_BLOCK; i++) {
            mb[i] = buf[i];
        }
        // Luma blocks use the Y table, the last two blocks (U, V) their own
        for (i = 0; i < blocks; i++) {
            NJpeg_DecodeBlock(&mb[i * NJPEG_BLOCK], qt[(i < blocks - 2) ? 0 : (i == blocks - 2) ? 1 : 2]);
        }
        if (mode == 2) {
            const s16* cu = &mb[4 * NJPEG_BLOCK];
            const s16* cv = &mb[5 * NJPEG_BLOCK];

            for (y = 0; y < 16; y++) {
                for (x = 0; x < 16; x++) {
                    const s16* luma = &mb[((y >> 3) * 2 + (x >> 3)) * NJPEG_BLOCK];
                    s32 c = (y >> 1) * 8 + (x >> 1);

                    out[y * 16 + x] = NJpeg_Pixel(luma[(y & 7) * 8 + (x & 7)], cu[c], cv[c]);
                }
            }
        } else {
            const s16* cu = &mb[2 * NJPEG_BLOCK];
            const s16* cv = &mb[3 * NJPEG_BLOCK];

            for (y = 0; y < 8; y++) {
                for (x = 0; x < 16; x++) {
                    const s16* luma = &mb[(x >> 3) * NJPEG_BLOCK];
                    s32 c = y * 8 + (x >> 1);

                    out[y * 16 + x] = NJpeg_Pixel(luma[y * 8 + (x & 7)], cu[c], cv[c]);
                }
            }
        }
    }
}

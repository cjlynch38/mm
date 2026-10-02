/**
 * C replacement for the libultra matrix routines written in MIPS assembly (src/libultra/mgu/):
 * guMtxIdent, guMtxIdentF, guMtxL2F, guMtxF2L, guNormalize, guScale and guTranslate.
 *
 * An Mtx holds 16 s15.16 fixed-point values: the 16-bit integer parts of all elements in words
 * 0x00-0x1F, then the 16-bit fractional parts in words 0x20-0x3F, each word holding two
 * horizontally adjacent elements (high half first). Like the assembly, this code builds whole
 * 32-bit words with shifts and masks, so the result does not depend on host endianness.
 *
 * Float to fixed conversion is mul.s by 65536.0f then trunc.w.s. For NaN or values whose
 * scaled magnitude reaches 2^31, the VR4300 raises an Unimplemented Operation exception (a
 * crash on N64). Here they convert to 0x7FFFFFFF, the MIPS default result for an invalid
 * conversion, as in fp.c, instead of PowerPC fctiwz's saturation.
 */
#include "ultra64.h"

#define MTX_WORD_INTPART 0  // word index of the integer parts
#define MTX_WORD_FRACPART 8 // word index of the fractional parts

// mul.s by 65536.0f then trunc.w.s
static inline u32 Mgu_FloatToFixed(f32 x) {
    f32 scaled = x * 65536.0f;

    // compared as doubles: -2^31 - 1 is not representable as an f32
    if (!((f64)scaled > -2147483649.0 && (f64)scaled < 2147483648.0)) {
        return 0x7FFFFFFF;
    }
    return (u32)(s32)scaled;
}

static inline u32* Mgu_Words(Mtx* m) {
    return (u32*)&m->m[0][0];
}

void guMtxIdent(Mtx* m) {
    u32* w = Mgu_Words(m);
    s32 i;

    for (i = 0; i < 16; i++) {
        w[i] = 0;
    }
    // integer parts of [0][0], [1][1], [2][2] and [3][3]
    w[0] = 0x10000;
    w[2] = 1;
    w[5] = 0x10000;
    w[7] = 1;
}

void guMtxIdentF(float mf[4][4]) {
    s32 i;
    s32 j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            mf[i][j] = (i == j) ? 1.0f : 0.0f;
        }
    }
}

void guMtxF2L(float mf[4][4], Mtx* m) {
    u32* w = Mgu_Words(m);
    const f32* src = &mf[0][0];
    s32 i;

    for (i = 0; i < 8; i++) {
        u32 e1 = Mgu_FloatToFixed(src[2 * i + 0]);
        u32 e2 = Mgu_FloatToFixed(src[2 * i + 1]);

        w[MTX_WORD_INTPART + i] = (e1 & 0xFFFF0000) | (e2 >> 16);
        w[MTX_WORD_FRACPART + i] = (e1 << 16) | (e2 & 0xFFFF);
    }
}

void guMtxL2F(float mf[4][4], Mtx* m) {
    u32* w = Mgu_Words(m);
    f32* dst = &mf[0][0];
    s32 i;

    for (i = 0; i < 8; i++) {
        u32 intPart = w[MTX_WORD_INTPART + i];
        u32 fracPart = w[MTX_WORD_FRACPART + i];
        s32 e1 = (s32)((intPart & 0xFFFF0000) | (fracPart >> 16));
        s32 e2 = (s32)((intPart << 16) | (fracPart & 0xFFFF));

        // cvt.s.w (round to nearest) then an exact scale by 2^-16
        dst[2 * i + 0] = (f32)e1 * (1.0f / 65536.0f);
        dst[2 * i + 1] = (f32)e2 * (1.0f / 65536.0f);
    }
}

void guNormalize(float* x, float* y, float* z) {
    // The assembly loads all three components before storing any, so aliased pointers behave the same
    f32 vx = *x;
    f32 vy = *y;
    f32 vz = *z;
    // (x*x + y*y) + z*z, sqrt.s, then the reciprocal (div.s 1.0) times each component.
    // A zero vector gives NaNs here; on N64 the 0 * inf trapped (FCSR invalid enable).
    f32 lenSq = (vx * vx + vy * vy) + vz * vz;
    f32 invLen = 1.0f / sqrtf(lenSq);

    *x = vx * invLen;
    *y = vy * invLen;
    *z = vz * invLen;
}

void guScale(Mtx* m, f32 x, f32 y, f32 z) {
    u32* w = Mgu_Words(m);
    u32 e;

    e = Mgu_FloatToFixed(x); // [0][0]: high halves of words 0x00 / 0x20
    w[0] = e & 0xFFFF0000;
    w[8] = e << 16;

    e = Mgu_FloatToFixed(y); // [1][1]: low halves of words 0x08 / 0x28
    w[2] = e >> 16;
    w[10] = e & 0xFFFF;

    e = Mgu_FloatToFixed(z); // [2][2]: high halves of words 0x14 / 0x34
    w[5] = e & 0xFFFF0000;
    w[13] = e << 16;

    w[7] = 1; // [3][3] = 1.0

    w[1] = 0;
    w[3] = 0;
    w[4] = 0;
    w[6] = 0;
    w[9] = 0;
    w[11] = 0;
    w[12] = 0;
    w[14] = 0;
    w[15] = 0;
}

void guTranslate(Mtx* m, f32 x, f32 y, f32 z) {
    u32* w = Mgu_Words(m);
    u32 ex = Mgu_FloatToFixed(x);
    u32 ey = Mgu_FloatToFixed(y);
    u32 ez;

    // [3][0] and [3][1]: words 0x18 / 0x38
    w[6] = (ex & 0xFFFF0000) | (ey >> 16);
    w[14] = (ex << 16) | (ey & 0xFFFF);

    // [3][2] and [3][3] = 1.0: words 0x1C / 0x3C
    ez = Mgu_FloatToFixed(z);
    w[7] = (ez & 0xFFFF0000) + 1;
    w[15] = ez << 16;

    w[1] = 0;
    w[3] = 0;
    w[4] = 0;
    w[8] = 0;
    w[9] = 0;
    w[10] = 0;
    w[11] = 0;
    w[12] = 0;
    w[13] = 0;

    // diagonal [0][0], [1][1], [2][2] = 1.0
    w[0] = 0x10000;
    w[5] = 0x10000;
    w[2] = 1;
}

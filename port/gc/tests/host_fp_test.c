/**
 * Host-side test for port/gc/game/fp.c (src/boot/libc64/fp.s) and port/gc/game/mgu.c
 * (src/libultra/mgu/), built with the host gcc inside WSL from the repository root:
 *
 *   gcc -O2 -std=gnu17 -fwrapv -fno-strict-aliasing -ffp-contract=off -fopenmp -Wall -Wextra \
 *       -Iinclude -o /tmp/host_fp_test port/gc/tests/host_fp_test.c -lm
 *   /tmp/host_fp_test            # every f32 bit pattern plus 200M doubles (about 25 s on 20 threads)
 *   /tmp/host_fp_test --quick    # every 61st f32 bit pattern, 20M doubles
 *
 * The decomp headers assume a 32-bit long, so this file defines the few types fp.c and mgu.c use
 * and includes them directly; their own #includes resolve to the real headers and are skipped by
 * the include guards defined below.
 *
 * What is compared:
 *  fp.c, bit for bit (so -0.0 vs +0.0 counts), on every f32 and on random/edge-case f64s:
 *   - against a reference model of the VR4300 instructions: the exact integer from libm
 *     floor/ceil/trunc/nearbyint (round to nearest even), converted as cvt.w does: values outside
 *     [-2^31, 2^31 - 1], NaN and infinity give 0x7FFFFFFF (the MIPS default result for an invalid
 *     conversion; real hardware raises an Unimplemented Operation exception there, see fp.c);
 *     round* = floor.w(x + 0.5) with the add rounded to the argument's precision;
 *   - against a hand-written table of expected results (signed zeros, halfway cases, 2^23, 2^24
 *     and 2^31 boundaries, NaN, infinities).
 *  mgu.c, bit for bit:
 *   - against literal transliterations of each mgu .s file (register by register, trunc.w.s
 *     modelled as above), on all inputs including out-of-range values;
 *   - against the C versions of these functions in libultra 2.0's gu sources (guMtxF2L/guMtxL2F/
 *     guMtxIdentF/guMtxIdent from mtxutil.c, guScaleF + guMtxF2L, guTranslateF + guMtxF2L,
 *     guNormalize from normalize.c, as also found in the zeldaret/oot decomp) and against this
 *     decomp's MtxConv_F2L (src/boot/libu64/mtxuty-cvt.c), on in-range inputs (the C versions
 *     use a plain (long) cast, which is undefined out of range);
 *   - guMtxL2F(guMtxF2L(m)) and guMtxF2L(guMtxL2F(m)) round trips where they are exact.
 */
#include <math.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef float f32;
typedef double f64;

typedef union {
    s32 m[4][4];
    struct {
        u16 intPart[4][4];
        u16 fracPart[4][4];
    };
    long long force_structure_alignment;
} Mtx;

#define FIXED_POINT_H // include/libc64/fixed_point.h
#define ULTRA64_H     // include/ultra64.h
#include "../game/fp.c"
#include "../game/mgu.c"

extern f32 gPositiveInfinity, gNegativeInfinity, gPositiveZero, gNegativeZero;
extern f32 qNaN0x3FFFFF, qNaN0x10000, sNaN0x3FFFFF;

static long sFailures;
static long sChecks;

#define REPORT(...)                            \
    do {                                       \
        long n_;                               \
        _Pragma("omp atomic capture") n_ = ++sFailures; \
        if (n_ <= 40) {                        \
            _Pragma("omp critical(print)")     \
            printf(__VA_ARGS__);               \
        }                                      \
    } while (0)

static u32 F32Bits(f32 f) {
    u32 w;
    memcpy(&w, &f, 4);
    return w;
}

static uint64_t F64Bits(f64 d) {
    uint64_t w;
    memcpy(&w, &d, 8);
    return w;
}

static f32 BitsF32(u32 w) {
    f32 f;
    memcpy(&f, &w, 4);
    return f;
}

static f64 BitsF64(uint64_t w) {
    f64 d;
    memcpy(&d, &w, 8);
    return d;
}

/* ---- reference model of the VR4300 conversions ---- */

// cvt to word of an already integral value (or NaN / infinity)
static s32 RefWord(f64 r) {
    if (isnan(r) || r < -2147483648.0 || r > 2147483647.0) {
        return 0x7FFFFFFF;
    }
    return (s32)r;
}

static s32 RefFloorW(f64 x) {
    return RefWord(floor(x));
}
static s32 RefCeilW(f64 x) {
    return RefWord(ceil(x));
}
static s32 RefTruncW(f64 x) {
    return RefWord(trunc(x));
}
static s32 RefRoundW(f64 x) {
    return RefWord(nearbyint(x)); // default rounding mode: nearest, ties to even
}

#define CHECK_F32(name, in, got, want)                                                              \
    if (F32Bits(got) != F32Bits(want)) {                                                            \
        REPORT("%s(%a [%08X]) = %a [%08X], want %a [%08X]\n", name, (f64)(in), F32Bits(in),          \
               (f64)(got), F32Bits(got), (f64)(want), F32Bits(want));                               \
    }
#define CHECK_F64(name, in, got, want)                                                              \
    if (F64Bits(got) != F64Bits(want)) {                                                            \
        REPORT("%s(%a) = %a, want %a\n", name, (f64)(in), (f64)(got), (f64)(want));                 \
    }
#define CHECK_S32(name, in, got, want)                                                              \
    if ((got) != (want)) {                                                                          \
        REPORT("%s(%a) = %d [%08X], want %d [%08X]\n", name, (f64)(in), (int)(got), (u32)(got),    \
               (int)(want), (u32)(want));                                                           \
    }

// All 20 functions on an f64 input (and the f32 ones too when x is exactly an f32)
static void CheckF64Input(f64 x) {
    s32 fw = RefFloorW(x);
    s32 cw = RefCeilW(x);
    s32 tw = RefTruncW(x);
    s32 rw = RefRoundW(x);
    s32 rdw = RefFloorW(x + 0.5);

    CHECK_F64("floor", x, n64_floor(x), (f64)fw);
    CHECK_S32("lfloor", x, n64_lfloor(x), fw);
    CHECK_F64("ceil", x, n64_ceil(x), (f64)cw);
    CHECK_S32("lceil", x, n64_lceil(x), cw);
    CHECK_F64("trunc", x, n64_trunc(x), (f64)tw);
    CHECK_S32("ltrunc", x, n64_ltrunc(x), tw);
    CHECK_F64("nearbyint", x, n64_nearbyint(x), (f64)rw);
    CHECK_S32("lnearbyint", x, n64_lnearbyint(x), rw);
    CHECK_F64("round", x, n64_round(x), (f64)rdw);
    CHECK_S32("lround", x, n64_lround(x), rdw);
}

static void CheckF32Input(f32 x) {
    f64 xd = x;
    s32 fw = RefFloorW(xd);
    s32 cw = RefCeilW(xd);
    s32 tw = RefTruncW(xd);
    s32 rw = RefRoundW(xd);
    volatile f32 sum = x + 0.5f; // add.s
    s32 rdw = RefFloorW(sum);

    CHECK_F32("floorf", x, n64_floorf(x), (f32)fw);
    CHECK_S32("lfloorf", x, n64_lfloorf(x), fw);
    CHECK_F32("ceilf", x, n64_ceilf(x), (f32)cw);
    CHECK_S32("lceilf", x, n64_lceilf(x), cw);
    CHECK_F32("truncf", x, n64_truncf(x), (f32)tw);
    CHECK_S32("ltruncf", x, n64_ltruncf(x), tw);
    CHECK_F32("nearbyintf", x, n64_nearbyintf(x), (f32)rw);
    CHECK_S32("lnearbyintf", x, n64_lnearbyintf(x), rw);
    CHECK_F32("roundf", x, n64_roundf(x), (f32)rdw);
    CHECK_S32("lroundf", x, n64_lroundf(x), rdw);
}

/* ---- hand-written expectations ---- */

#define INVALID 0x7FFFFFFF
#define INT_MIN32 ((s32)0x80000000)

typedef struct {
    f32 x;
    s32 floorW, ceilW, truncW, roundEvenW, roundW; // lfloorf, lceilf, ltruncf, lnearbyintf, lroundf
} F32Case;

static void CheckTables(void) {
    static const F32Case cases[] = {
        { 0.0f, 0, 0, 0, 0, 0 },
        { -0.0f, 0, 0, 0, 0, 0 },
        { 0.5f, 0, 1, 0, 0, 1 },
        { -0.5f, -1, 0, 0, 0, 0 },
        { 1.5f, 1, 2, 1, 2, 2 },
        { -1.5f, -2, -1, -1, -2, -1 },
        { 2.5f, 2, 3, 2, 2, 3 },
        { -2.5f, -3, -2, -2, -2, -2 },
        { 0.49999997f, 0, 1, 0, 0, 1 }, // roundf: 0.49999997f + 0.5f rounds to 1.0f
        { -0.49999997f, -1, 0, 0, 0, 0 },
        { 1e-30f, 0, 1, 0, 0, 0 },
        { -1e-30f, -1, 0, 0, 0, 0 },
        { 1e-45f, 0, 1, 0, 0, 0 }, // denormal
        { -1e-45f, -1, 0, 0, 0, 0 },
        { 8388607.5f, 8388607, 8388608, 8388607, 8388608, 8388608 },
        { 8388606.5f, 8388606, 8388607, 8388606, 8388606, 8388607 },
        { -8388607.5f, -8388608, -8388607, -8388607, -8388608, -8388607 },
        { 8388608.0f, 8388608, 8388608, 8388608, 8388608, 8388608 }, // 8388608.5f ties to even
        { 8388609.0f, 8388609, 8388609, 8388609, 8388609, 8388610 }, // 8388609.5f ties to 8388610
        { 16777216.0f, 16777216, 16777216, 16777216, 16777216, 16777216 },
        { 16777218.0f, 16777218, 16777218, 16777218, 16777218, 16777218 },
        { 2147483520.0f, 2147483520, 2147483520, 2147483520, 2147483520, 2147483520 },
        { 2147483648.0f, INVALID, INVALID, INVALID, INVALID, INVALID },
        { -2147483648.0f, INT_MIN32, INT_MIN32, INT_MIN32, INT_MIN32, INT_MIN32 },
        { -2147483904.0f, INVALID, INVALID, INVALID, INVALID, INVALID },
        { 3e38f, INVALID, INVALID, INVALID, INVALID, INVALID },
        { -3e38f, INVALID, INVALID, INVALID, INVALID, INVALID },
    };
    static const f32 specials[] = { __builtin_inff(), -__builtin_inff(), __builtin_nanf(""), -__builtin_nanf(""),
                                    __builtin_nansf("1") };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const F32Case* c = &cases[i];

        CHECK_S32("table lfloorf", c->x, n64_lfloorf(c->x), c->floorW);
        CHECK_S32("table lceilf", c->x, n64_lceilf(c->x), c->ceilW);
        CHECK_S32("table ltruncf", c->x, n64_ltruncf(c->x), c->truncW);
        CHECK_S32("table lnearbyintf", c->x, n64_lnearbyintf(c->x), c->roundEvenW);
        CHECK_S32("table lroundf", c->x, n64_lroundf(c->x), c->roundW);
        // the float variants are cvt.s.w of the same word: never -0.0
        CHECK_F32("table floorf", c->x, n64_floorf(c->x), (f32)c->floorW);
        CHECK_F32("table ceilf", c->x, n64_ceilf(c->x), (f32)c->ceilW);
        CHECK_F32("table truncf", c->x, n64_truncf(c->x), (f32)c->truncW);
        CHECK_F32("table nearbyintf", c->x, n64_nearbyintf(c->x), (f32)c->roundEvenW);
        CHECK_F32("table roundf", c->x, n64_roundf(c->x), (f32)c->roundW);
        CHECK_S32("table lfloor", c->x, n64_lfloor(c->x), c->floorW);
        CHECK_S32("table lnearbyint", c->x, n64_lnearbyint(c->x), c->roundEvenW);
        sChecks += 12;
    }
    for (i = 0; i < sizeof(specials) / sizeof(specials[0]); i++) {
        f32 x = specials[i];

        CHECK_S32("special lfloorf", x, n64_lfloorf(x), INVALID);
        CHECK_S32("special lceilf", x, n64_lceilf(x), INVALID);
        CHECK_S32("special ltruncf", x, n64_ltruncf(x), INVALID);
        CHECK_S32("special lnearbyintf", x, n64_lnearbyintf(x), INVALID);
        CHECK_S32("special lroundf", x, n64_lroundf(x), INVALID);
        CHECK_F32("special floorf", x, n64_floorf(x), 2147483648.0f);
        CHECK_F64("special floor", (f64)x, n64_floor(x), 2147483647.0);
        CHECK_F64("special round", (f64)x, n64_round(x), 2147483647.0);
        sChecks += 8;
    }

    // f64-only boundaries
    CHECK_S32("lfloor", -2147483648.5, n64_lfloor(-2147483648.5), INVALID);
    CHECK_S32("lfloor", -2147483648.0, n64_lfloor(-2147483648.0), INT_MIN32);
    CHECK_S32("lfloor", 2147483647.99, n64_lfloor(2147483647.99), 2147483647);
    CHECK_S32("lceil", -2147483648.99, n64_lceil(-2147483648.99), INT_MIN32);
    CHECK_S32("lceil", -2147483649.0, n64_lceil(-2147483649.0), INVALID);
    CHECK_S32("lceil", 2147483646.01, n64_lceil(2147483646.01), 2147483647);
    CHECK_S32("lceil", 2147483647.01, n64_lceil(2147483647.01), INVALID);
    CHECK_S32("ltrunc", -2147483648.99, n64_ltrunc(-2147483648.99), INT_MIN32);
    CHECK_S32("ltrunc", 2147483647.99, n64_ltrunc(2147483647.99), 2147483647);
    CHECK_S32("ltrunc", 2147483648.0, n64_ltrunc(2147483648.0), INVALID);
    CHECK_S32("lnearbyint", -2147483648.5, n64_lnearbyint(-2147483648.5), INT_MIN32);
    CHECK_S32("lnearbyint", -2147483647.5, n64_lnearbyint(-2147483647.5), INT_MIN32);
    CHECK_S32("lnearbyint", -2147483648.51, n64_lnearbyint(-2147483648.51), INVALID);
    CHECK_S32("lnearbyint", 2147483646.5, n64_lnearbyint(2147483646.5), 2147483646);
    CHECK_S32("lnearbyint", 2147483647.49, n64_lnearbyint(2147483647.49), 2147483647);
    CHECK_S32("lnearbyint", 2147483647.5, n64_lnearbyint(2147483647.5), INVALID);
    CHECK_S32("lround", 2147483646.5, n64_lround(2147483646.5), 2147483647);
    CHECK_S32("lround", 2147483647.5, n64_lround(2147483647.5), INVALID);
    CHECK_S32("lround", -2147483648.5, n64_lround(-2147483648.5), INT_MIN32);
    CHECK_S32("lround", -2147483648.51, n64_lround(-2147483648.51), INVALID);
    CHECK_F64("nearbyint", -0.5, n64_nearbyint(-0.5), 0.0);
    CHECK_F64("floor", -0.0, n64_floor(-0.0), 0.0);
    sChecks += 22;

    // fp.s data
    CHECK_S32("gPositiveInfinity", 0.0, F32Bits(gPositiveInfinity), 0x7F800000u);
    CHECK_S32("gNegativeInfinity", 0.0, F32Bits(gNegativeInfinity), 0xFF800000u);
    CHECK_S32("gPositiveZero", 0.0, F32Bits(gPositiveZero), 0x00000000u);
    CHECK_S32("gNegativeZero", 0.0, F32Bits(gNegativeZero), 0x80000000u);
    CHECK_S32("qNaN0x3FFFFF", 0.0, F32Bits(qNaN0x3FFFFF), 0x7FBFFFFFu);
    CHECK_S32("qNaN0x10000", 0.0, F32Bits(qNaN0x10000), 0x7F810000u);
    CHECK_S32("sNaN0x3FFFFF", 0.0, F32Bits(sNaN0x3FFFFF), 0x7FFFFFFFu);
    sChecks += 7;
}

/* ---- random numbers ---- */

static uint64_t Rand64(uint64_t* s) {
    // xorshift64*
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 0x2545F4914F6CDD1DULL;
}

static f64 RandUnit(uint64_t* s) {
    return (Rand64(s) >> 11) * (1.0 / 9007199254740992.0);
}

// Doubles that exercise the interesting parts of the range
static f64 RandDouble(uint64_t* s) {
    static const f64 kBounds[] = { 2147483648.0, 2147483647.0, 2147483647.5, 2147483648.5, 2147483649.0,
                                   0.0,          0.5,          1.0,          8388608.0,    4503599627370496.0 };
    static const f64 kFracs[] = { 0.0, 0.5, 0.25, 0.75, 0.49999999999999994, 0.5000000000000001, 1e-9 };
    uint64_t r = Rand64(s);
    f64 x;

    switch (r & 7) {
        case 0: // any bit pattern
            return BitsF64(Rand64(s));
        case 1: // integer +- a fraction, |x| < 2^33
        case 2:
            x = (f64)(int64_t)(Rand64(s) % (1ULL << 34)) - (f64)(1ULL << 33);
            x += kFracs[(r >> 3) % 7] * (((r >> 8) & 1) ? -1.0 : 1.0);
            return x;
        case 3: // random fraction, |x| < 2^32
            return (RandUnit(s) * 2.0 - 1.0) * 4294967296.0;
        case 4: // within a few ulps of a boundary
        case 5: {
            int steps = (int)((r >> 3) % 9) - 4;
            x = kBounds[(r >> 8) % 10];
            if ((r >> 12) & 1) {
                x = -x;
            }
            while (steps > 0) {
                x = nextafter(x, INFINITY), steps--;
            }
            while (steps < 0) {
                x = nextafter(x, -INFINITY), steps++;
            }
            return x;
        }
        case 6: // small values
            return (RandUnit(s) * 2.0 - 1.0) * 8.0;
        default: // boundary +- random fraction
            x = kBounds[(r >> 8) % 10] + (RandUnit(s) * 2.0 - 1.0) * 2.0;
            return ((r >> 12) & 1) ? -x : x;
    }
}

/* ---- mgu: literal transliterations of the .s files ---- */

static u32 AsmTruncWS(f32 f) {
    return (u32)RefTruncW(f); // trunc.w.s (+ mfc1)
}

// mtxf2l.s (a0 = mf, a1 = m)
static void Asm_guMtxF2L(const f32* a0, u32* a1) {
    const f32 fv0 = 65536.0f;
    const u32 t9 = 0xFFFF0000;
    u32* t8 = a1 + 0x20 / 4;

    do {
        f32 ft0 = a0[0];
        volatile f32 ft1 = ft0 * fv0;
        u32 t0 = AsmTruncWS(ft1);
        f32 ft3 = a0[1];
        volatile f32 ft4 = ft3 * fv0;
        u32 t1 = AsmTruncWS(ft4);
        u32 t2 = t0 & t9;
        u32 t3 = t1 >> 0x10;
        u32 t4 = t2 | t3;
        u32 t5;
        u32 t6;

        a1[0] = t4;
        t5 = t0 << 0x10;
        t6 = t1 & 0xFFFF;
        a1[0x20 / 4] = t5 | t6;
        a1 += 1;
        a0 += 2;
    } while (a1 != t8);
}

// mtxl2f.s (a0 = mf, a1 = m)
static void Asm_guMtxL2F(f32* a0, const u32* a1) {
    const f32 fv0 = 0.0000152587890625f;
    const u32 t9 = 0xFFFF0000;
    const u32* t8 = a1 + 0x20 / 4;

    do {
        u32 t0 = a1[0];
        u32 t1 = a1[0x20 / 4];
        u32 t4 = (t0 & t9) | (t1 >> 0x10);
        u32 t7 = (t0 << 0x10) | (t1 & 0xFFFF);
        volatile f32 ft1 = (f32)(s32)t4; // cvt.s.w
        volatile f32 ft4 = (f32)(s32)t7;

        a0[0] = ft1 * fv0;
        a0[1] = ft4 * fv0;
        a0 += 2;
        a1 += 1;
    } while (a1 != t8);
}

// mtxident.s
static void Asm_guMtxIdent(u32* a0) {
    u32 t0 = 0 + 1;
    u32 t1 = t0 << 0x10;
    static const int zeros[] = { 0x04, 0x0C, 0x10, 0x18, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34, 0x38, 0x3C };
    size_t i;

    a0[0x00 / 4] = t1;
    a0[0x08 / 4] = t0;
    a0[0x14 / 4] = t1;
    a0[0x1C / 4] = t0;
    for (i = 0; i < 12; i++) {
        a0[zeros[i] / 4] = 0;
    }
}

// mtxidentf.s
static void Asm_guMtxIdentF(u32* a0) {
    u32 t0 = 0x3F800000; // li.s t0, 1.0
    int off;

    for (off = 0; off < 0x40; off += 4) {
        a0[off / 4] = 0;
    }
    a0[0x00 / 4] = t0;
    a0[0x14 / 4] = t0;
    a0[0x28 / 4] = t0;
    a0[0x3C / 4] = t0;
}

// normalize.s
static void Asm_guNormalize(f32* a0, f32* a1, f32* a2) {
    volatile f32 ft0 = *a0;
    volatile f32 ft1 = *a1;
    volatile f32 ft2 = *a2;
    volatile f32 ft3 = ft0 * ft0;
    volatile f32 ft4 = ft1 * ft1;
    volatile f32 ft5 = ft3 + ft4;

    ft4 = ft2 * ft2;
    ft3 = ft4 + ft5;
    ft5 = 1.0f;
    ft4 = sqrtf(ft3); // sqrt.s: correctly rounded
    ft3 = ft5 / ft4;
    ft4 = ft0 * ft3;
    ft5 = ft1 * ft3;
    ft0 = ft2 * ft3;
    *a0 = ft4;
    *a1 = ft5;
    *a2 = ft0;
}

// scale.s
static void Asm_guScale(u32* a0, f32 a1, f32 a2, f32 a3) {
    const f32 ft0 = 65536.0f;
    u32 t1;
    static const int zeros[] = { 0x04, 0x0C, 0x10, 0x18, 0x24, 0x2C, 0x30, 0x38, 0x3C };
    size_t i;

    t1 = AsmTruncWS(a1 * ft0);
    a0[0x00 / 4] = (t1 >> 0x10) << 0x10;
    a0[0x20 / 4] = t1 << 0x10;

    t1 = AsmTruncWS(a2 * ft0);
    a0[0x08 / 4] = t1 >> 0x10;
    a0[0x28 / 4] = t1 & 0xFFFF;

    t1 = AsmTruncWS(a3 * ft0);
    a0[0x14 / 4] = (t1 >> 0x10) << 0x10;
    a0[0x34 / 4] = t1 << 0x10;

    a0[0x1C / 4] = 1;
    for (i = 0; i < 9; i++) {
        a0[zeros[i] / 4] = 0;
    }
}

// translate.s
static void Asm_guTranslate(u32* a0, f32 a1, f32 a2, f32 a3) {
    const f32 ft0 = 65536.0f;
    u32 t0;
    u32 t1;
    u32 t2;
    u32 t3;
    static const int zeros[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x20, 0x24, 0x28, 0x2C, 0x30, 0x34 };
    size_t i;

    t1 = AsmTruncWS(a1 * ft0);
    t3 = AsmTruncWS(a2 * ft0);
    t2 = t1 >> 0x10;
    t0 = t2 << 0x10;
    t2 = t3 >> 0x10;
    t0 = t0 | t2;
    a0[0x18 / 4] = t0;
    t0 = t1 << 0x10;
    t2 = t3 << 0x10;
    t2 = t2 >> 0x10;
    t0 = t0 | t2;
    a0[0x38 / 4] = t0;

    t1 = AsmTruncWS(a3 * ft0);
    t2 = t1 >> 0x10;
    t0 = t2 << 0x10;
    t0 = t0 + 1;
    a0[0x1C / 4] = t0;
    t2 = t1 << 0x10;
    a0[0x3C / 4] = t2;

    for (i = 0; i < 12; i++) {
        a0[zeros[i] / 4] = 0;
    }
    a0[0x00 / 4] = 0x00010000;
    a0[0x14 / 4] = 0x00010000;
    a0[0x08 / 4] = 0x00000001;
}

/* ---- mgu: libultra 2.0 C versions (gu/mtxutil.c, scale.c, translate.c, normalize.c) ---- */

#define FTOFIX32(x) (s32)((x) * (float)0x00010000)
#define FIX32TOF(x) ((float)(x) * (1.0f / (float)0x00010000))

static void Ref_guMtxF2L(float mf[4][4], Mtx* m) {
    int i, j;
    s32 e1, e2;
    s32* ai = &m->m[0][0];
    s32* af = &m->m[2][0];

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            e1 = FTOFIX32(mf[i][j * 2]);
            e2 = FTOFIX32(mf[i][j * 2 + 1]);
            *(ai++) = (e1 & 0xFFFF0000) | ((e2 >> 16) & 0xFFFF);
            *(af++) = ((e1 << 16) & 0xFFFF0000) | (e2 & 0xFFFF);
        }
    }
}

static void Ref_guMtxL2F(float mf[4][4], Mtx* m) {
    int i, j;
    u32 e1, e2;
    u32* ai = (u32*)&m->m[0][0];
    u32* af = (u32*)&m->m[2][0];
    s32 q1, q2;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 2; j++) {
            e1 = (*ai & 0xFFFF0000) | ((*af >> 16) & 0xFFFF);
            e2 = ((*(ai++) << 16) & 0xFFFF0000) | (*(af++) & 0xFFFF);
            q1 = (s32)e1;
            q2 = (s32)e2;
            mf[i][j * 2] = FIX32TOF(q1);
            mf[i][j * 2 + 1] = FIX32TOF(q2);
        }
    }
}

static void Ref_guMtxIdentF(float mf[4][4]) {
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (i == j) {
                mf[i][j] = 1.0;
            } else {
                mf[i][j] = 0.0;
            }
        }
    }
}

static void Ref_guMtxIdent(Mtx* m) {
    float mf[4][4];

    Ref_guMtxIdentF(mf);
    Ref_guMtxF2L(mf, m);
}

static void Ref_guScale(Mtx* m, float x, float y, float z) {
    float mf[4][4];

    Ref_guMtxIdentF(mf); // guScaleF
    mf[0][0] = x;
    mf[1][1] = y;
    mf[2][2] = z;
    mf[3][3] = 1;
    Ref_guMtxF2L(mf, m);
}

static void Ref_guTranslate(Mtx* m, float x, float y, float z) {
    float mf[4][4];

    Ref_guMtxIdentF(mf); // guTranslateF
    mf[3][0] = x;
    mf[3][1] = y;
    mf[3][2] = z;
    Ref_guMtxF2L(mf, m);
}

static void Ref_guNormalize(float* x, float* y, float* z) {
    float m;

    m = 1 / sqrtf((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    *x *= m;
    *y *= m;
    *z *= m;
}

// src/boot/libu64/mtxuty-cvt.c MtxConv_F2L, with the u16 halves packed big-endian into words
static void Ref_MtxConvF2L(u32* words, float mf[4][4]) {
    u16 intPart[4][4];
    u16 fracPart[4][4];
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            s32 value = (mf[i][j] * 0x10000);

            intPart[i][j] = value >> 16;
            fracPart[i][j] = value;
        }
    }
    for (i = 0; i < 8; i++) {
        words[i] = ((u32)intPart[i / 2][(i % 2) * 2] << 16) | intPart[i / 2][(i % 2) * 2 + 1];
        words[8 + i] = ((u32)fracPart[i / 2][(i % 2) * 2] << 16) | fracPart[i / 2][(i % 2) * 2 + 1];
    }
}

/* ---- mgu checks ---- */

static int IsNaNBits(u32 w) {
    return ((w & 0x7F800000) == 0x7F800000) && ((w & 0x007FFFFF) != 0);
}

// Compares n words bit for bit; with nanAny, two NaNs match whatever their payloads
static void CompareWords(const char* what, const u32* got, const u32* want, int n, const f32* inputs, int numInputs,
                         int nanAny) {
    int i;

    for (i = 0; i < n; i++) {
        if ((got[i] != want[i]) && !(nanAny && IsNaNBits(got[i]) && IsNaNBits(want[i]))) {
            if (numInputs > 0) {
                REPORT("%s: word %d = %08X, want %08X (first input %a, %d inputs)\n", what, i, got[i], want[i],
                       (f64)inputs[0], numInputs);
            } else {
                REPORT("%s: word %d = %08X, want %08X\n", what, i, got[i], want[i]);
            }
            return;
        }
    }
}

static void CheckWords(const char* what, const u32* got, const u32* want, int n, const f32* inputs, int numInputs) {
    CompareWords(what, got, want, n, inputs, numInputs, 0);
}

// NaN payloads depend on which operand the host FPU propagates; the N64 does not define them either
static void CheckFloatsNaNAny(const char* what, const f32* got, const f32* want, int n, const f32* inputs) {
    CompareWords(what, (const u32*)got, (const u32*)want, n, inputs, 3, 1);
}

// True if x * 65536 converts without overflow (where the C references are defined)
static int FixInRange(f32 x) {
    f32 scaled = x * 65536.0f;

    return (scaled > -2147483649.0) && (scaled < 2147483648.0);
}

// Random f32 for matrix elements: mostly in the s15.16 range, sometimes out of it or special
static f32 RandMtxFloat(uint64_t* s) {
    static const f32 kSpecials[] = { 0.0f,       -0.0f,       1.0f,       -1.0f,      32767.99998f,
                                     -32767.99998f, 32768.0f, -32768.0f, 32768.004f, -32768.004f,
                                     1.0f / 65536.0f, -1.0f / 65536.0f, 1.5f / 65536.0f, -1.5f / 65536.0f,
                                     1e30f,      -1e30f,      __builtin_inff(), -__builtin_inff(), __builtin_nanf("") };
    uint64_t r = Rand64(s);

    switch (r & 7) {
        case 0:
            return kSpecials[(r >> 3) % (sizeof(kSpecials) / sizeof(kSpecials[0]))];
        case 1:
            return BitsF32((u32)(Rand64(s) >> 32));
        case 2:
        case 3:
            return (f32)((RandUnit(s) * 2.0 - 1.0) * 2.0);
        case 4:
            return (f32)((RandUnit(s) * 2.0 - 1.0) * 32768.0);
        case 5:
            return (f32)((s32)(Rand64(s) >> 32)) / 65536.0f; // exact s15.16 values
        default:
            return (f32)((RandUnit(s) * 2.0 - 1.0) * 400.0);
    }
}

static void CheckMgu(long iterations) {
    uint64_t seed = 0x9E3779B97F4A7C15ULL;
    long it;
    Mtx got;
    Mtx want;
    f32 mf[4][4];
    f32 mfGot[4][4];
    f32 mfWant[4][4];

    // identity matrices
    memset(&got, 0xA5, sizeof(got));
    guMtxIdent(&got);
    Asm_guMtxIdent((u32*)want.m);
    CheckWords("guMtxIdent vs mtxident.s", (u32*)got.m, (u32*)want.m, 16, NULL, 0);
    Ref_guMtxIdent(&want);
    CheckWords("guMtxIdent vs libultra C", (u32*)got.m, (u32*)want.m, 16, NULL, 0);
    memset(mfGot, 0xA5, sizeof(mfGot));
    guMtxIdentF(mfGot);
    Asm_guMtxIdentF((u32*)mfWant);
    CheckWords("guMtxIdentF vs mtxidentf.s", (u32*)mfGot, (u32*)mfWant, 16, NULL, 0);
    Ref_guMtxIdentF(mfWant);
    CheckWords("guMtxIdentF vs libultra C", (u32*)mfGot, (u32*)mfWant, 16, NULL, 0);
    sChecks += 4;

    for (it = 0; it < iterations; it++) {
        int i;
        int inRange = 1;
        u32 words[16];
        f32 v[3];
        f32 w[3];

        for (i = 0; i < 16; i++) {
            mf[i / 4][i % 4] = RandMtxFloat(&seed);
            inRange &= FixInRange(mf[i / 4][i % 4]);
        }

        // guMtxF2L
        memset(&got, 0xA5, sizeof(got));
        guMtxF2L(mf, &got);
        Asm_guMtxF2L(&mf[0][0], (u32*)want.m);
        CheckWords("guMtxF2L vs mtxf2l.s", (u32*)got.m, (u32*)want.m, 16, mf[0], 16);
        if (inRange) {
            Ref_guMtxF2L(mf, &want);
            CheckWords("guMtxF2L vs libultra C", (u32*)got.m, (u32*)want.m, 16, mf[0], 16);
            Ref_MtxConvF2L(words, mf);
            CheckWords("guMtxF2L vs MtxConv_F2L", (u32*)got.m, words, 16, mf[0], 16);
            // the fixed-point value is the float truncated to a multiple of 2^-16
            guMtxL2F(mfGot, &got);
            for (i = 0; i < 16; i++) {
                f32 x = mf[i / 4][i % 4];
                f32 t = (f32)(trunc((f64)x * 65536.0) / 65536.0);

                if (fabsf(x) < 128.0f && F32Bits(mfGot[i / 4][i % 4]) != F32Bits(t + 0.0f)) {
                    REPORT("guMtxL2F(guMtxF2L(%a)) = %a, want %a\n", (f64)x, (f64)mfGot[i / 4][i % 4], (f64)t);
                }
            }
        }

        // guMtxL2F on arbitrary words. On odd iterations every element is within +-2^23 (in units of
        // 2^-16), so the float conversion is exact and guMtxF2L(guMtxL2F(m)) must give m back.
        for (i = 0; i < 16; i++) {
            got.m[i / 4][i % 4] = (s32)(Rand64(&seed) >> 32);
        }
        if (it & 1) {
            u32* gw = (u32*)got.m;

            for (i = 0; i < 8; i++) {
                s32 e1 = (s32)(Rand64(&seed) >> 40) - (1 << 23);
                s32 e2 = (s32)(Rand64(&seed) >> 40) - (1 << 23);

                gw[i] = ((u32)e1 & 0xFFFF0000) | ((u32)e2 >> 16);
                gw[8 + i] = ((u32)e1 << 16) | ((u32)e2 & 0xFFFF);
            }
        }
        memcpy(&want, &got, sizeof(Mtx));
        memset(mfGot, 0xA5, sizeof(mfGot));
        guMtxL2F(mfGot, &got);
        Asm_guMtxL2F(&mfWant[0][0], (u32*)got.m);
        CheckWords("guMtxL2F vs mtxl2f.s", (u32*)mfGot, (u32*)mfWant, 16, NULL, 0);
        Ref_guMtxL2F(mfWant, &got);
        CheckWords("guMtxL2F vs libultra C", (u32*)mfGot, (u32*)mfWant, 16, NULL, 0);
        if (it & 1) {
            Mtx back;

            guMtxF2L(mfGot, &back);
            CheckWords("guMtxF2L(guMtxL2F(m)) round trip", (u32*)back.m, (u32*)want.m, 16, NULL, 0);
        }

        // guScale / guTranslate
        v[0] = RandMtxFloat(&seed);
        v[1] = RandMtxFloat(&seed);
        v[2] = RandMtxFloat(&seed);
        inRange = FixInRange(v[0]) && FixInRange(v[1]) && FixInRange(v[2]);

        memset(&got, 0xA5, sizeof(got));
        guScale(&got, v[0], v[1], v[2]);
        Asm_guScale((u32*)want.m, v[0], v[1], v[2]);
        CheckWords("guScale vs scale.s", (u32*)got.m, (u32*)want.m, 16, v, 3);
        if (inRange) {
            Ref_guScale(&want, v[0], v[1], v[2]);
            CheckWords("guScale vs libultra C", (u32*)got.m, (u32*)want.m, 16, v, 3);
        }

        memset(&got, 0xA5, sizeof(got));
        guTranslate(&got, v[0], v[1], v[2]);
        Asm_guTranslate((u32*)want.m, v[0], v[1], v[2]);
        CheckWords("guTranslate vs translate.s", (u32*)got.m, (u32*)want.m, 16, v, 3);
        if (inRange) {
            Ref_guTranslate(&want, v[0], v[1], v[2]);
            CheckWords("guTranslate vs libultra C", (u32*)got.m, (u32*)want.m, 16, v, 3);
        }

        // guNormalize (NaN results compare by bit pattern too)
        memcpy(w, v, sizeof(w));
        guNormalize(&v[0], &v[1], &v[2]);
        {
            f32 a[3];
            f32 b[3];

            memcpy(a, w, sizeof(a));
            memcpy(b, w, sizeof(b));
            Asm_guNormalize(&a[0], &a[1], &a[2]);
            CheckFloatsNaNAny("guNormalize vs normalize.s", v, a, 3, w);
            Ref_guNormalize(&b[0], &b[1], &b[2]);
            CheckFloatsNaNAny("guNormalize vs libultra C", v, b, 3, w);
            // aliased arguments: all loads happen before the stores
            memcpy(a, w, sizeof(a));
            memcpy(b, w, sizeof(b));
            guNormalize(&a[0], &a[0], &a[1]);
            Asm_guNormalize(&b[0], &b[0], &b[1]);
            CheckFloatsNaNAny("guNormalize aliased vs normalize.s", a, b, 2, w);
        }
        sChecks += 14;
    }
}

int main(int argc, char** argv) {
    int quick = (argc > 1) && (strcmp(argv[1], "--quick") == 0);
    uint64_t stride = quick ? 61 : 1;
    long numDoubles = quick ? 20000000L : 200000000L;
    long mguIterations = quick ? 200000L : 2000000L;
    long long i;

    CheckTables();
    printf("tables: %ld failures\n", sFailures);

    // every f32 bit pattern (or every 61st), all 20 functions
#pragma omp parallel for schedule(dynamic, 1 << 16)
    for (i = 0; i < (1LL << 32); i += (long long)stride) {
        f32 x = BitsF32((u32)i);

        CheckF32Input(x);
        CheckF64Input(x);
    }
    sChecks += (long)((1LL << 32) / (long long)stride) * 20;
    printf("f32 sweep: %ld failures so far\n", sFailures);

    // random and boundary doubles
#pragma omp parallel
    {
        uint64_t seed = 0x243F6A8885A308D3ULL;
        long j;

#ifdef _OPENMP
        seed += (uint64_t)omp_get_thread_num() * 0x9E3779B97F4A7C15ULL;
#endif
#pragma omp for
        for (j = 0; j < numDoubles; j++) {
            CheckF64Input(RandDouble(&seed));
        }
    }
    sChecks += numDoubles * 10;
    printf("f64 random: %ld failures so far\n", sFailures);

    CheckMgu(mguIterations);
    printf("mgu: %ld failures so far\n", sFailures);

    printf("%s: %ld checks, %ld failures\n", (sFailures == 0) ? "PASS" : "FAIL", sChecks, sFailures);
    return (sFailures == 0) ? 0 : 1;
}

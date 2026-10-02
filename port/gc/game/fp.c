/**
 * C replacement for src/boot/libc64/fp.s: the game's rounding functions and float constants.
 *
 * Each N64 function is a single VR4300 conversion to a 32-bit integer (floor.w, ceil.w,
 * trunc.w or round.w), followed by cvt.s.w / cvt.d.w for the variants returning a float.
 * The results therefore differ from C99 floorf() and friends:
 *  - every result passes through an s32, so -0.0 and negative inputs that round to zero give
 *    +0.0 (floorf(-0.0f) == +0.0f, ceilf(-0.5f) == +0.0f, truncf(-0.5f) == +0.0f);
 *  - nearbyint* round halfway cases to even (round.w always rounds to nearest-even, whatever
 *    the FCSR rounding mode);
 *  - round* compute floor(x + 0.5) with an IEEE single (double) add, so
 *    roundf(0.49999997f) == 1.0f and roundf(-2.5f) == -2.0f.
 *
 * Invalid conversions (NaN, infinity, or a rounded value outside [-2^31, 2^31 - 1]): the
 * VR4300 raises an Unimplemented Operation exception (FCSR cause bit E, which cannot be
 * masked), and libultra's __osException treats it as a fault, so the game stops on the crash
 * screen. Shipping game paths never get there. These functions instead return the MIPS
 * architecture's default result for an invalid conversion, 0x7FFFFFFF (the float variants
 * return 2147483648.0f), not PowerPC fctiw[z]'s saturation to 0x80000000 for negative and
 * NaN inputs. Denormal inputs (also an E exception on the VR4300) are treated as ordinary
 * small values.
 *
 * The functions are built as n64_floorf etc. because newlib's libm defines most of these names
 * with C99 semantics. gc_prelude.h renames the game's declarations and calls; that also stops
 * GCC from treating them as builtins.
 */
#include "libc64/fixed_point.h"

/*
 * fp.s .data. The NaN names follow the MIPS NaN encoding, where a set fraction MSB marks a
 * signalling NaN. PowerPC uses the opposite convention; the bit patterns are kept unchanged.
 */
f32 gPositiveInfinity = __builtin_inff();      // 0x7F800000
f32 gNegativeInfinity = -__builtin_inff();     // 0xFF800000
f32 gPositiveZero = 0.0f;                      // 0x00000000
f32 gNegativeZero = -0.0f;                     // 0x80000000
f32 qNaN0x3FFFFF = __builtin_nansf("0x3FFFFF"); // 0x7FBFFFFF
f32 qNaN0x10000 = __builtin_nansf("0x10000");   // 0x7F810000
f32 sNaN0x3FFFFF = __builtin_nanf("0x3FFFFF");  // 0x7FFFFFFF

// MIPS default result of an invalid conversion to a 32-bit integer
#define CVT_W_INVALID 0x7FFFFFFF

/*
 * The conversions below work on doubles: every f32 is exactly representable as an f64, and the
 * range limits (such as -2^31 - 1) are not representable as f32. Inside the checked ranges the
 * (s32) cast, which truncates toward zero, cannot overflow.
 */

// floor.w.fmt
static inline s32 Fp_FloorW(f64 x) {
    s32 i;

    if (!(x >= -2147483648.0 && x < 2147483648.0)) {
        return CVT_W_INVALID;
    }
    i = (s32)x;
    if ((f64)i > x) {
        i--;
    }
    return i;
}

// ceil.w.fmt
static inline s32 Fp_CeilW(f64 x) {
    s32 i;

    if (!(x > -2147483649.0 && x <= 2147483647.0)) {
        return CVT_W_INVALID;
    }
    i = (s32)x;
    if ((f64)i < x) {
        i++;
    }
    return i;
}

// trunc.w.fmt
static inline s32 Fp_TruncW(f64 x) {
    if (!(x > -2147483649.0 && x < 2147483648.0)) {
        return CVT_W_INVALID;
    }
    return (s32)x;
}

// round.w.fmt: round to nearest, ties to even
static inline s32 Fp_RoundW(f64 x) {
    s32 i;
    f64 frac;

    // -2147483648.5 rounds to the even -2^31; 2147483647.5 rounds to 2^31, out of range
    if (!(x >= -2147483648.5 && x < 2147483647.5)) {
        return CVT_W_INVALID;
    }
    i = (s32)x;
    frac = x - (f64)i; // exact: the fractional part of x
    if ((frac > 0.5) || ((frac == 0.5) && (i & 1))) {
        i++;
    } else if ((frac < -0.5) || ((frac == -0.5) && (i & 1))) {
        i--;
    }
    return i;
}

f32 n64_floorf(f32 x) {
    return (f32)Fp_FloorW(x);
}

f64 n64_floor(f64 x) {
    return (f64)Fp_FloorW(x);
}

s32 n64_lfloorf(f32 x) {
    return Fp_FloorW(x);
}

s32 n64_lfloor(f64 x) {
    return Fp_FloorW(x);
}

f32 n64_ceilf(f32 x) {
    return (f32)Fp_CeilW(x);
}

f64 n64_ceil(f64 x) {
    return (f64)Fp_CeilW(x);
}

s32 n64_lceilf(f32 x) {
    return Fp_CeilW(x);
}

s32 n64_lceil(f64 x) {
    return Fp_CeilW(x);
}

f32 n64_truncf(f32 x) {
    return (f32)Fp_TruncW(x);
}

f64 n64_trunc(f64 x) {
    return (f64)Fp_TruncW(x);
}

s32 n64_ltruncf(f32 x) {
    return Fp_TruncW(x);
}

s32 n64_ltrunc(f64 x) {
    return Fp_TruncW(x);
}

f32 n64_nearbyintf(f32 x) {
    return (f32)Fp_RoundW(x);
}

f64 n64_nearbyint(f64 x) {
    return (f64)Fp_RoundW(x);
}

s32 n64_lnearbyintf(f32 x) {
    return Fp_RoundW(x);
}

s32 n64_lnearbyint(f64 x) {
    return Fp_RoundW(x);
}

/*
 * round*: add 0.5 (add.s / add.d, rounded to the argument's precision), then floor.w.
 * Assigning to a variable of the argument's type keeps that intermediate rounding.
 */
f32 n64_roundf(f32 x) {
    f32 sum = x + 0.5f;

    return (f32)Fp_FloorW(sum);
}

f64 n64_round(f64 x) {
    f64 sum = x + 0.5;

    return (f64)Fp_FloorW(sum);
}

s32 n64_lroundf(f32 x) {
    f32 sum = x + 0.5f;

    return Fp_FloorW(sum);
}

s32 n64_lround(f64 x) {
    f64 sum = x + 0.5;

    return Fp_FloorW(sum);
}

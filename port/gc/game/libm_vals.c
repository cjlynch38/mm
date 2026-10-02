/**
 * C replacement for src/libultra/gu/libm_vals.s: the NaN that libultra's sinf/cosf return for
 * infinite or NaN arguments.
 *
 * 0x7F810000 is a quiet NaN in the MIPS encoding (fraction MSB clear) but a signalling NaN on
 * PowerPC. Copying it with lfs/stfs keeps the bits; FPU arithmetic on it sets FPSCR[VXSNAN] and
 * yields a quiet NaN, which only traps if invalid-operation exceptions are enabled.
 * The assembly puts it in .rdata; it is ordinary data here because guint.h declares it non-const.
 */
#include "PR/guint.h"

f32 __libm_qnan_f = __builtin_nansf("0x10000"); // 0x7F810000

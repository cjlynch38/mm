/**
 * Stand-in for the decomp's PR/ultratypes.h in the audio host tests: the decomp's version defines u32
 * and s32 as long, which is 64 bits on x86-64. Same names, fixed-width types.
 */
#ifndef PR_ULTRATYPES_H
#define PR_ULTRATYPES_H

#include <stddef.h>
#include <stdint.h>

typedef int8_t s8;
typedef uint8_t u8;
typedef int16_t s16;
typedef uint16_t u16;
typedef int32_t s32;
typedef uint32_t u32;
typedef int64_t s64;
typedef uint64_t u64;
typedef float f32;
typedef double f64;

#endif

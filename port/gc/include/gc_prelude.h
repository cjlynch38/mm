/**
 * Force-included (-include) into every game translation unit of the GameCube
 * build (Makefile.gc). Keep this file free of libogc headers: game code uses
 * the decomp's own headers and types.
 */
#ifndef GC_PRELUDE_H
#define GC_PRELUDE_H

#ifndef TARGET_GC
#error "gc_prelude.h is only for the GameCube build"
#endif

/*
 * The game defines functions whose names newlib, libogc and libfat also define
 * or call. Rename the game's copies so library code (for example the SD card
 * driver calling malloc() and usleep()) never binds to the game's N64
 * implementations, and the game never binds to the libraries' versions.
 */

// src/boot/libc64/malloc.c: the game's system-arena allocator
#define malloc mm_malloc
#define malloc_r mm_malloc_r
#define calloc mm_calloc
#define realloc mm_realloc
#define free mm_free

// src/boot/libc64/sprintf.c
#define sprintf mm_sprintf
#define vsprintf mm_vsprintf

// src/boot/libc64/sleep.c: implemented with the N64 OS timer
#define csleep mm_csleep
#define nsleep mm_nsleep
#define usleep mm_usleep
#define msleep mm_msleep
#define sleep mm_sleep

// src/libultra/gu: N64 sine/cosine. sinf.c/cosf.c define __sinf/__cosf plus weak sinf/cosf
// aliases via #pragma weak (not macro-expanded); Makefile.gc strips those aliases.
#define sinf __sinf
#define cosf __cosf

// src/libultra/gu: N64 matrix helpers (libogc has functions with the same names)
#define guLookAt n64_guLookAt
#define guOrtho n64_guOrtho
#define guPerspective n64_guPerspective

/*
 * ---- game-asm renames ----
 * C replacements for the game's MIPS assembly live in port/gc/game. fp.c replaces
 * src/boot/libc64/fp.s (declared in include/libc64/fixed_point.h) and keeps the N64 semantics:
 * results pass through a 32-bit integer and nearbyint rounds ties to even. newlib's libm defines
 * floor, floorf, ceil, ceilf, trunc, truncf, round, roundf, lround, lroundf, nearbyint and
 * nearbyintf with C99 semantics, so all 20 functions get n64_ names. The rename also stops GCC
 * from treating the calls as builtins. It renames every game identifier with these names, e.g.
 * the member SSList floor in z64bgcheck.h becomes n64_floor (consistently, in all game TUs).
 */
#define floorf n64_floorf
#define floor n64_floor
#define lfloorf n64_lfloorf
#define lfloor n64_lfloor
#define ceilf n64_ceilf
#define ceil n64_ceil
#define lceilf n64_lceilf
#define lceil n64_lceil
#define truncf n64_truncf
#define trunc n64_trunc
#define ltruncf n64_ltruncf
#define ltrunc n64_ltrunc
#define nearbyintf n64_nearbyintf
#define nearbyint n64_nearbyint
#define lnearbyintf n64_lnearbyintf
#define lnearbyint n64_lnearbyint
#define roundf n64_roundf
#define round n64_round
#define lroundf n64_lroundf
#define lround n64_lround

#endif

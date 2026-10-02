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

#endif

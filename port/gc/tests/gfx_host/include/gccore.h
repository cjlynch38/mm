/**
 * Stand-in for libogc's gccore.h in the gfx host test: gfx_internal.h includes it, but the code under
 * test (gfx_rsp.c) uses only C types from it.
 */
#ifndef GFX_HOST_FAKE_GCCORE_H
#define GFX_HOST_FAKE_GCCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#endif

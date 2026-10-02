/**
 * Buffers the N64 build places at fixed RAM addresses: the system heap and the crash screen
 * framebuffer. See gc_game.h.
 */
#include "ultra64.h"
#include "alignment.h"
#include "assert.h"
#include "macros.h"
#include "gc_game.h"

static_assert(GC_FAULT_FB_PIXELS == SCREEN_WIDTH * SCREEN_HEIGHT, "GC_FAULT_FB_PIXELS must match the screen size");

u8 gGcSystemHeap[GC_SYSTEM_HEAP_SIZE] ALIGNED(32);
GcFaultArea gGcFaultArea ALIGNED(64);

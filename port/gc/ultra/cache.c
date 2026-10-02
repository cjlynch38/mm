/**
 * libultra cache maintenance (osInvalDCache, osInvalICache, osWritebackDCache,
 * osWritebackDCacheAll) for the GameCube: all no-ops.
 *
 * On N64 these keep the CPU caches coherent with RCP/PI DMA. Here every "DMA" (ROM reads, flash,
 * controller data) is a CPU copy, and no hardware reads game memory in M2, so there is nothing to
 * keep coherent. Invalidating would be actively harmful: DmaMgr_DmaRomToRam calls osInvalDCache
 * after the copy, and DCInvalidateRange would discard the freshly copied bytes still in the cache
 * (and GC cache lines are 32 bytes, N64 lines 16, so neighbouring data would be lost as well).
 * Overlays are linked statically, so the instruction cache never sees new code either.
 * Revisit when GX or AI DMA consume game buffers (M3/M4): writeback must then become DCFlushRange.
 */
#include "gc_ultra_internal.h"

void osInvalDCache(void* vaddr, size_t nbytes) {
}

void osInvalICache(void* vaddr, size_t nbytes) {
}

void osWritebackDCache(void* vaddr, s32 nbytes) {
}

void osWritebackDCacheAll(void) {
}

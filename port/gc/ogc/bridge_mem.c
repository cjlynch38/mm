/**
 * gc_mem_alloc: permanent allocations for shim-owned buffers (thread stacks, the ROM cache, the
 * resident audio ROM range), carved downwards from the top of the MEM1 arena.
 *
 * newlib's sbrk grows the heap up from SYS_GetArena1Lo() and stops at SYS_GetArena1Hi(), so
 * lowering Arena1Hi hands memory to the shim that malloc can never reach. Shim buffers end up at
 * the top of MEM1 (0x81xxxxxx) while the game image, its static heaps and anything malloc'd stay
 * low, below 0x81000000. Both sbrk and this function update the arena with interrupts disabled,
 * which on a single CPU makes them atomic with respect to each other.
 */
#include <gccore.h>
#include <ogc/machine/processor.h>
#include "gc_ogc.h"

/* Allocations at least this large are logged */
#define MEM_LOG_THRESHOLD 0x10000

/* More than all of MEM1 can never succeed; refusing early also keeps the rounding from wrapping */
#define MEM_MAX_SIZE 0x01800000u

static unsigned int sArenaTop;
static unsigned int sCarved;

void* gc_mem_alloc(unsigned int size, unsigned int align) {
    unsigned int level;
    unsigned int hi;
    unsigned int lo;
    unsigned int newHi;

    if ((align & (align - 1)) != 0) {
        gc_log("gc_mem_alloc: alignment %u is not a power of two", align);
        return NULL;
    }
    if (align < 32) {
        align = 32;
    }
    if (size > MEM_MAX_SIZE) {
        gc_log("gc_mem_alloc: %u bytes is more than MEM1", size);
        return NULL;
    }
    size = (size + 31) & ~31u;

    _CPU_ISR_Disable(level);
    hi = (unsigned int)SYS_GetArena1Hi();
    lo = (unsigned int)SYS_GetArena1Lo();
    if (sArenaTop == 0) {
        sArenaTop = hi;
    }
    newHi = (hi - size) & ~(align - 1);
    if (size > hi - lo || newHi < lo) {
        _CPU_ISR_Restore(level);
        gc_log("gc_mem_alloc: out of memory (%u bytes wanted, %u free)", size, hi - lo);
        return NULL;
    }
    SYS_SetArena1Hi((void*)newHi);
    sCarved += hi - newHi;
    _CPU_ISR_Restore(level);

    if (size >= MEM_LOG_THRESHOLD) {
        gc_log("mem: %u KB at %08X (shim total %u KB, arena %u KB free)", size / 1024, newHi, sCarved / 1024,
               (newHi - lo) / 1024);
    }
    return (void*)newHi;
}

unsigned int gc_ogc_mem_carved(void) {
    return sCarved;
}

unsigned int gc_ogc_mem_top(void) {
    return (sArenaTop != 0) ? sArenaTop : (unsigned int)SYS_GetArena1Hi();
}

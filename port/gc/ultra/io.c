/**
 * Emulated RCP register block for IO_READ/IO_WRITE (include/PR/rcp.h under TARGET_GC).
 *
 * The game touches RCP registers directly in a few places: sched.c halts the RSP when cancelling
 * a task and spins until SP_STATUS reports HALT, CIC6105.c reads SP DMEM and CIC addresses for
 * its fault page, and the excluded aisetnextbuf.c programs the AI. With the RCP stubbed, the RSP
 * is always halted and every other register reads as zero.
 */
#include "gc_ultra_internal.h"

u32 __gcIoRead(u32 addr) {
    switch (addr) {
        case SP_STATUS_REG:
            return SP_STATUS_HALT;

        default:
            return 0;
    }
}

void __gcIoWrite(u32 addr, u32 data) {
    // Writes have no effect: SP_SET_HALT is already true and nothing else is emulated.
}

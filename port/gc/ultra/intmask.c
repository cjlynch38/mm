/**
 * libultra interrupt masking: osSetIntMask, osGetIntMask.
 *
 * The game only ever masks everything (OS_IM_NONE) around short list edits and restores the mask it
 * got back. The mask is virtual: going from any enabled mask to OS_IM_NONE disables CPU interrupts
 * (and with them preemption) through the bridge, and going back restores the level saved at that
 * point. Like the N64 status register, the mask belongs to the calling thread.
 */
#include "gc_os_core.h"

// No interrupt line enabled at all
#define IM_IS_DISABLED(im) (((im) & (OS_IM_ALL & ~OS_IM_NONE)) == 0)

OSIntMask osSetIntMask(OSIntMask im) {
    unsigned int level = gc_irq_disable();
    GcIntMaskState* state = __gcGetIntMaskState();
    OSIntMask prev = state->mask;

    state->mask = im;

    if (IM_IS_DISABLED(im)) {
        // Stay disabled. Remember the level to restore only on the outermost transition.
        if (!IM_IS_DISABLED(prev)) {
            state->irqLevel = level;
        }
    } else if (IM_IS_DISABLED(prev)) {
        gc_irq_restore(state->irqLevel);
    } else {
        gc_irq_restore(level);
    }

    return prev;
}

OSIntMask osGetIntMask(void) {
    return __gcGetIntMaskState()->mask;
}

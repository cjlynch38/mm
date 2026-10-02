#ifndef SEGMENTED_ADDRESS_H
#define SEGMENTED_ADDRESS_H

#include "ultra64.h"

#include "stdint.h"

extern uintptr_t gSegments[NUM_SEGMENTS];

#ifdef TARGET_GC
// GameCube RAM extends to 0x81800000, so a CPU address at or above 0x81000000 would decode as segment 1.
// CPU addresses (bit 31 set) are passed through unchanged; real segmented addresses never have bit 31 set.
static inline void* Gc_SegmentedToK0(uintptr_t addr) {
    if (addr & 0x80000000) {
        return (void*)addr;
    }
    return (void*)((gSegments[SEGMENT_NUMBER(addr)] + K0BASE) + SEGMENT_OFFSET(addr));
}
#define SEGMENTED_TO_K0(addr) Gc_SegmentedToK0((uintptr_t)(addr))
#else
#define SEGMENTED_TO_K0(addr) (void*)((gSegments[SEGMENT_NUMBER(addr)] + K0BASE) + SEGMENT_OFFSET(addr))
#endif

#endif

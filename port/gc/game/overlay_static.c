/**
 * Static overlays: replaces src/boot/libu64/loadfragment*.c (MIPS overlay loading and relocation).
 *
 * Every overlay is linked into the executable at its own VRAM address, and the overlay tables use
 * loadedRamAddr == vramStart (see the TARGET_GC patches in z_actor.c, z_effect_soft_sprite.c, z_DLF.c,
 * z_kaleido_manager.c and z_overlay.c). On N64 each load copies the overlay fresh from ROM, so its
 * .data is back to its initial values and its .bss is zero; actors rely on this (e.g. one-shot
 * "textures desegmented" flags). Overlay_Load reproduces that from a pristine copy of every
 * overlay's .data taken before any overlay code runs.
 */
#include "ultra64.h"
#include "libu64/loadfragment.h"
#include "string.h"
#include "gc_bridge.h"
#include "gc_overlay_ranges.h"

s32 gOverlayLogSeverity = 2;

static u8* sDataSnapshot = NULL;
static u32* sDataSnapshotOffsets = NULL;

static void Gc_OverlayStaticInit(void) {
    u32 total = 0;
    s32 i;

    for (i = 0; i < gGcOverlayRangeCount; i++) {
        total += gGcOverlayRanges[i].dataEnd - gGcOverlayRanges[i].dataStart;
    }

    sDataSnapshot = gc_mem_alloc(total, 32);
    sDataSnapshotOffsets = gc_mem_alloc(gGcOverlayRangeCount * sizeof(u32), 4);
    if ((sDataSnapshot == NULL) || (sDataSnapshotOffsets == NULL)) {
        gc_halt("overlay_static: cannot allocate %u bytes for the overlay data snapshot", (unsigned)total);
    }

    total = 0;
    for (i = 0; i < gGcOverlayRangeCount; i++) {
        const GcOverlayRange* range = &gGcOverlayRanges[i];
        u32 size = range->dataEnd - range->dataStart;

        sDataSnapshotOffsets[i] = total;
        memcpy(sDataSnapshot + total, range->dataStart, size);
        total += size;
    }

    gc_log("overlay_static: %d overlays, %u bytes of initial data saved", (int)gGcOverlayRangeCount, (unsigned)total);
}

static s32 Gc_FindOverlay(void* vramStart) {
    s32 lo = 0;
    s32 hi = gGcOverlayRangeCount - 1;

    while (lo <= hi) {
        s32 mid = (lo + hi) / 2;
        u8* start = gGcOverlayRanges[mid].start;

        if ((u8*)vramStart == start) {
            return mid;
        }
        if ((u8*)vramStart < start) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return -1;
}

size_t Overlay_Load(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd, void* allocatedRamAddr) {
    const GcOverlayRange* range;
    s32 index;

    // The first load happens before any overlay code has run, so the data is still pristine
    if (sDataSnapshot == NULL) {
        Gc_OverlayStaticInit();
    }

    if (allocatedRamAddr != vramStart) {
        gc_halt("Overlay_Load: %p loaded at %p, expected its VRAM address", vramStart, allocatedRamAddr);
    }

    index = Gc_FindOverlay(vramStart);
    if (index < 0) {
        gc_halt("Overlay_Load: no overlay starts at %p (vrom %08x)", vramStart, (unsigned)vromStart);
    }

    range = &gGcOverlayRanges[index];
    memcpy(range->dataStart, sDataSnapshot + sDataSnapshotOffsets[index], range->dataEnd - range->dataStart);
    bzero(range->bssStart, range->bssEnd - range->bssStart);

    if (gOverlayLogSeverity >= 3) {
        gc_log("Overlay_Load: %s", range->name);
    }

    return (uintptr_t)vramEnd - (uintptr_t)vramStart;
}

void* Overlay_AllocateAndLoad(uintptr_t vromStart, uintptr_t vromEnd, void* vramStart, void* vramEnd) {
    Overlay_Load(vromStart, vromEnd, vramStart, vramEnd, vramStart);
    return vramStart;
}

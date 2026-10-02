#include "kaleido_manager.h"

#include "global.h"
#include "fault.h"
#include "libu64/loadfragment.h"

#define KALEIDO_OVERLAY(name) \
    { NULL, ROM_FILE(ovl_##name), SEGMENT_START(ovl_##name), SEGMENT_END(ovl_##name), 0, #name }

KaleidoMgrOverlay gKaleidoMgrOverlayTable[KALEIDO_OVL_MAX] = {
    KALEIDO_OVERLAY(kaleido_scope),
    KALEIDO_OVERLAY(player_actor),
};

void* sKaleidoAreaPtr = NULL;
KaleidoMgrOverlay* gKaleidoMgrCurOvl = NULL;

FaultAddrConvClient sKaleidoMgrFaultAddrConvClient;

uintptr_t KaleidoManager_FaultAddrConv(uintptr_t address, void* param) {
    uintptr_t addr = address;
    KaleidoMgrOverlay* kaleidoMgrOvl = gKaleidoMgrCurOvl;
    uintptr_t ramConv;
    void* ramStart;
    size_t diff;

    if (kaleidoMgrOvl != NULL) {
        diff = (uintptr_t)kaleidoMgrOvl->vramEnd - (uintptr_t)kaleidoMgrOvl->vramStart;
        ramStart = kaleidoMgrOvl->loadedRamAddr;
        ramConv = (uintptr_t)kaleidoMgrOvl->vramStart - (uintptr_t)ramStart;

        if (ramStart != NULL) {
            if ((addr >= (uintptr_t)ramStart) && (addr < (uintptr_t)ramStart + diff)) {
                return addr + ramConv;
            }
        }
    }
    return 0;
}

void KaleidoManager_LoadOvl(KaleidoMgrOverlay* ovl) {
#ifdef TARGET_GC
    // Linked at its VRAM address; Overlay_Load resets its data and bss (port/gc/game/overlay_static.c).
    // The pause menu and player overlays share sKaleidoAreaPtr on N64, so each load starts fresh there too.
    ovl->loadedRamAddr = ovl->vramStart;
#else
    ovl->loadedRamAddr = sKaleidoAreaPtr;
#endif
    Overlay_Load(ovl->file.vromStart, ovl->file.vromEnd, ovl->vramStart, ovl->vramEnd, ovl->loadedRamAddr);
    ovl->offset = (uintptr_t)ovl->loadedRamAddr - (uintptr_t)ovl->vramStart;
    gKaleidoMgrCurOvl = ovl;
}

void KaleidoManager_ClearOvl(KaleidoMgrOverlay* ovl) {
    if (ovl->loadedRamAddr != NULL) {
        ovl->offset = 0;
#ifndef TARGET_GC // On GameCube the area is the overlay's own code; the next load resets its data
        bzero(ovl->loadedRamAddr, (uintptr_t)ovl->vramEnd - (uintptr_t)ovl->vramStart);
#endif
        ovl->loadedRamAddr = NULL;
        gKaleidoMgrCurOvl = NULL;
    }
}

void KaleidoManager_Init(PlayState* play) {
    s32 largestSize = 0;
    s32 size;
    u32 i;

    for (i = 0; i < ARRAY_COUNT(gKaleidoMgrOverlayTable); i++) {
        size = (uintptr_t)gKaleidoMgrOverlayTable[i].vramEnd - (uintptr_t)gKaleidoMgrOverlayTable[i].vramStart;
        if (size > largestSize) {
            largestSize = size;
        }
    }

#ifdef TARGET_GC
    // No shared load area needed; keep the pointer non-NULL for code that checks it
    sKaleidoAreaPtr = gKaleidoMgrOverlayTable[0].vramStart;
#else
    sKaleidoAreaPtr = THA_AllocTailAlign16(&play->state.tha, largestSize);
#endif
    gKaleidoMgrCurOvl = NULL;
    Fault_AddAddrConvClient(&sKaleidoMgrFaultAddrConvClient, KaleidoManager_FaultAddrConv, NULL);
}

void KaleidoManager_Destroy(void) {
    Fault_RemoveAddrConvClient(&sKaleidoMgrFaultAddrConvClient);

    if (gKaleidoMgrCurOvl != NULL) {
        KaleidoManager_ClearOvl(gKaleidoMgrCurOvl);
        gKaleidoMgrCurOvl = NULL;
    }

    sKaleidoAreaPtr = NULL;
}

void* KaleidoManager_GetRamAddr(void* vram) {
    if (gKaleidoMgrCurOvl == NULL) {
        s32 pad[2];
        KaleidoMgrOverlay* ovl = &gKaleidoMgrOverlayTable[0];

        do {
            if (((uintptr_t)vram >= (uintptr_t)ovl->vramStart) && ((uintptr_t)ovl->vramEnd >= (uintptr_t)vram)) {
                KaleidoManager_LoadOvl(ovl);
                return (void*)((uintptr_t)vram + ovl->offset);
            }
            ovl++;
#ifdef AVOID_UB
            //! @bug The original loop ends when ovl reaches &sKaleidoAreaPtr, relying on it directly following
            //! gKaleidoMgrOverlayTable in memory.
        } while (ovl < &gKaleidoMgrOverlayTable[ARRAY_COUNT(gKaleidoMgrOverlayTable)]);
#else
        } while (ovl != (KaleidoMgrOverlay*)&sKaleidoAreaPtr);
#endif

        return NULL;
    } else if (((uintptr_t)vram < (uintptr_t)gKaleidoMgrCurOvl->vramStart) ||
               ((uintptr_t)vram >= (uintptr_t)gKaleidoMgrCurOvl->vramEnd)) {
        return NULL;
    }

    return (void*)((uintptr_t)vram + gKaleidoMgrCurOvl->offset);
}

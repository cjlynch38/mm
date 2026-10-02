/**
 * GameCube-only declarations used by TARGET_GC patches in the game's own source (src/, include/).
 * Compiled with the decomp's headers.
 */
#ifndef GC_GAME_H
#define GC_GAME_H

#include "PR/ultratypes.h"

/**
 * System heap. On N64 it spans from the end of the `buffers` segment to the framebuffers at the
 * fixed address 0x80780000 (0x3FDB40 bytes in n64-us). The GameCube build keeps the same size
 * in a statically allocated buffer, so it lives below 0x81000000 with the rest of the game.
 */
#define GC_SYSTEM_HEAP_SIZE 0x3FDB40
extern u8 gGcSystemHeap[GC_SYSTEM_HEAP_SIZE];

/**
 * Crash screen framebuffer. On N64 it is placed at the end of RAM (FAULT_FB_ADDRESS,
 * osMemSize - framebuffer size), which on GameCube is in the middle of the program.
 * sys_initial_check.c also writes error textures just below FAULT_FB_ADDRESS, hence the pad.
 * The address must be a constant expression (fault_drawer.c uses it in a static initializer).
 */
#define GC_FAULT_FB_PAD 0x10000
#define GC_FAULT_FB_PIXELS (320 * 240)

typedef struct {
    u8 pad[GC_FAULT_FB_PAD];
    u16 framebuffer[GC_FAULT_FB_PIXELS];
} GcFaultArea;

extern GcFaultArea gGcFaultArea;
#define GC_FAULT_FB_ADDRESS ((void*)gGcFaultArea.framebuffer)

/** Bring-up tracing (port/gc/game/trace.c), called from Graph_ThreadEntry. */
void Gc_TraceGameStateStart(s32 index, u32 size);
void Gc_TraceFrame(void);
void Gc_TraceGameStateEnd(void);
/** GC_AUTOSTART builds: true while Start should be held on controller 1 (title screen only). */
s32 Gc_AutoStartPressed(void);
/** Cutscene flag set by a cutscene script (title logo debugging). */
void Gc_TraceCutsceneFlag(s16 flag);

#endif

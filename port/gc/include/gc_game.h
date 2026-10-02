/**
 * GameCube-only declarations used by TARGET_GC patches in the game's own source (src/, include/).
 * Compiled with the decomp's headers.
 */
#ifndef GC_GAME_H
#define GC_GAME_H

#include "PR/ultratypes.h"
#include "PR/sptask.h"

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

/** Bring-up tracing (port/gc/game/trace.c), called from Graph_ThreadEntry. `gameState` is the new
 *  GameState (allocated and zeroed, not initialised yet). */
void Gc_TraceGameStateStart(s32 index, u32 size, void* gameState);
void Gc_TraceFrame(void);
void Gc_TraceGameStateEnd(void);
/** Around GameState_Update in Graph_ExecuteAndDraw: the game's logic and display list building per frame. */
void Gc_TraceUpdateBegin(void);
void Gc_TraceUpdateEnd(void);
/** The running GameState (NULL between gamestates), its gGameStateOverlayTable index (-1 before the first
 *  one) and the frames it has run. Game thread only. */
void* Gc_TraceCurGameState(s32* index, u32* frames);
/** The GC_AUTOSTART build option: 0 off, 1 press Start on the title screen, 2 play the input script. */
s32 Gc_AutoStartMode(void);
/** GC_AUTOSTART=1 builds: true while Start should be held on controller 1 (title screen only). */
s32 Gc_AutoStartPressed(void);
/** Cutscene flag set by a cutscene script (title logo debugging). */
void Gc_TraceCutsceneFlag(s16 flag);

/**
 * GC_AUTOSTART=2 builds: scripted controller input (port/gc/game/input_script.c).
 * Gc_InputScriptFrame runs on the game thread after every frame and decides the input for the next ones;
 * Gc_InputScriptGet is called by osContGetReadData (any thread) for controller 1 and returns true while
 * the script drives the controller, with the N64 buttons to press and the stick (stickSet false: leave
 * the stick alone).
 */
void Gc_InputScriptFrame(void);
s32 Gc_InputScriptGet(u16* buttons, s8* stickX, s8* stickY, s32* stickSet);

/** The RSP JPEG task (M_NJPEGTASK, z_jpeg.c's njpgdspMain) on the CPU (port/gc/game/njpeg_cpu.c), run by
 *  osSpTaskStartGo: the macroblocks of task->t.data_ptr's JpegTaskData become RGBA5551 tiles in place. */
void Gc_NJpegRunTask(OSTask* task);

#endif

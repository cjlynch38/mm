/**
 * Progress tracing for the GameCube bring-up: logs gamestate changes and the frame rate, so a run
 * with graphics stubbed can be followed in the on-screen console and in sd:/mmgcport/log.txt.
 */
#include "ultra64.h"
#include "stdbool.h"
#include "z64save.h"
#include "gc_bridge.h"
#include "gc_game.h"
#include "gc_options.h"

#define GC_GAMESTATE_PLAY 3
#define GC_GAMESTATE_FILE_SELECT 5

static const char* const sGameStateNames[] = {
    "Setup", "MapSelect", "ConsoleLogo", "Play", "TitleSetup", "FileSelect", "DayTelop",
};

static s32 sCurGameState = -1;
static u32 sFrames;
static u32 sFramesSinceReport;
static OSTime sReportTime;
static u32 sCutsceneFlagsLogged; // flags already logged in this gamestate (cutscenes set them every frame)

static const char* Gc_GameStateName(s32 index) {
    if ((index >= 0) && (index < (s32)(sizeof(sGameStateNames) / sizeof(sGameStateNames[0])))) {
        return sGameStateNames[index];
    }
    return "?";
}

void Gc_TraceGameStateStart(s32 index, u32 size) {
    sCurGameState = index;
    sFrames = 0;
    sFramesSinceReport = 0;
    sReportTime = osGetTime();
    sCutsceneFlagsLogged = 0;
    if (index == GC_GAMESTATE_PLAY) {
        gc_log("gamestate %d %s start (%u bytes): entrance %04X, scene layer %d, cutscene %04X", (int)index,
               Gc_GameStateName(index), (unsigned)size, (unsigned)gSaveContext.save.entrance,
               (int)gSaveContext.sceneLayer, (unsigned)gSaveContext.save.cutsceneIndex);
    } else {
        gc_log("gamestate %d %s start (%u bytes)", (int)index, Gc_GameStateName(index), (unsigned)size);
    }
}

void Gc_TraceFrame(void) {
    OSTime now;
    u32 elapsedUs;

    sFrames++;
    sFramesSinceReport++;

    now = osGetTime();
    elapsedUs = OS_CYCLES_TO_USEC(now - sReportTime);
    if (elapsedUs >= 1000000) {
        u32 fpsTimes10 = (sFramesSinceReport * 10000000ull) / elapsedUs;

        gc_log("%s frame %u, %u.%u fps", Gc_GameStateName(sCurGameState), (unsigned)sFrames,
               (unsigned)(fpsTimes10 / 10), (unsigned)(fpsTimes10 % 10));
        sFramesSinceReport = 0;
        sReportTime = now;
    }
}

void Gc_TraceGameStateEnd(void) {
    gc_log("gamestate %d %s end after %u frames", (int)sCurGameState, Gc_GameStateName(sCurGameState),
           (unsigned)sFrames);
}

void Gc_TraceCutsceneFlag(s16 flag) {
    if ((flag >= 0) && (flag < 32) && !(sCutsceneFlagsLogged & (1u << flag))) {
        sCutsceneFlagsLogged |= 1u << flag;
        gc_log("%s frame %u: cutscene flag %d set", Gc_GameStateName(sCurGameState), (unsigned)sFrames, (int)flag);
    }
}

/**
 * GC_AUTOSTART builds: during the title demo (Play before File Select was ever reached), tap Start for a
 * few frames every 2 seconds, so unattended runs reach File Select. Never presses anything afterwards.
 */
s32 Gc_AutoStartPressed(void) {
#if GC_AUTOSTART
    static s32 sReachedFileSelect = false;

    if (sCurGameState == GC_GAMESTATE_FILE_SELECT) {
        sReachedFileSelect = true;
    }
    if (sReachedFileSelect || (sCurGameState != GC_GAMESTATE_PLAY) || (sFrames < 60)) {
        return false;
    }
    return (sFrames % 40) < 4;
#else
    return false;
#endif
}

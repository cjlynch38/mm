/**
 * Progress tracing for the GameCube bring-up: logs gamestate changes and the frame rate, so a run
 * with graphics stubbed can be followed in the on-screen console and in sd:/mmgcport/log.txt.
 */
#include "ultra64.h"
#include "gc_bridge.h"
#include "gc_game.h"

static const char* const sGameStateNames[] = {
    "Setup", "MapSelect", "ConsoleLogo", "Play", "TitleSetup", "FileSelect", "DayTelop",
};

static s32 sCurGameState = -1;
static u32 sFrames;
static u32 sFramesSinceReport;
static OSTime sReportTime;

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
    gc_log("gamestate %d %s start (%u bytes)", (int)index, Gc_GameStateName(index), (unsigned)size);
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

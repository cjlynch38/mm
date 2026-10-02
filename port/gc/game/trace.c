/**
 * Progress tracing for the GameCube bring-up: logs gamestate changes and the frame rate, so a run
 * with graphics stubbed can be followed in the on-screen console and in sd:/mmgcport/log.txt.
 *
 * During Play it also logs what an unattended run needs to be followed from the log alone: the scene
 * and room once the scene is loaded and on every room change, message boxes (text id) as they open,
 * cutscene starts and ends, player form changes, and, in GC_AUTOSTART builds, once per second a status
 * line (player position, yaw, state flags, message and cutscene state, time of day). The frame rate line
 * also gives the time GameState_Update (the game's logic and display list building) took per frame.
 */
#include "ultra64.h"
#include "stdbool.h"
#include "z64play.h"
#include "z64player.h"
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
static void* sCurGameStatePtr;
static u32 sFrames;
static u32 sFramesSinceReport;
static OSTime sReportTime;
static u32 sCutsceneFlagsLogged; // flags already logged in this gamestate (cutscenes set them every frame)
static OSTime sUpdateStart;      // GameState_Update of the current frame started here
static u64 sUpdateTotalUs;       // GameState_Update time since the last report (wall time: includes the
static u32 sUpdateMaxUs;         // higher-priority threads that preempt it, gfx tasks and audio)

// Play state seen at the previous frame (-1: not seen yet), to log changes
static s32 sPlayScene;
static s32 sPlayRoom;
static s32 sPlayMsgMode;
static s32 sPlayTextId;
static s32 sPlayCsState;
static s32 sPlayForm;

static const char* Gc_GameStateName(s32 index) {
    if ((index >= 0) && (index < (s32)(sizeof(sGameStateNames) / sizeof(sGameStateNames[0])))) {
        return sGameStateNames[index];
    }
    return "?";
}

void Gc_TraceGameStateStart(s32 index, u32 size, void* gameState) {
    sCurGameState = index;
    sCurGameStatePtr = gameState;
    sFrames = 0;
    sFramesSinceReport = 0;
    sReportTime = osGetTime();
    sCutsceneFlagsLogged = 0;
    sUpdateTotalUs = 0;
    sUpdateMaxUs = 0;
    sPlayScene = sPlayRoom = sPlayMsgMode = sPlayTextId = sPlayCsState = sPlayForm = -1;
    if (index == GC_GAMESTATE_PLAY) {
        gc_log("gamestate %d %s start (%u bytes): entrance %04X, scene layer %d, cutscene %04X", (int)index,
               Gc_GameStateName(index), (unsigned)size, (unsigned)gSaveContext.save.entrance,
               (int)gSaveContext.sceneLayer, (unsigned)gSaveContext.save.cutsceneIndex);
    } else {
        gc_log("gamestate %d %s start (%u bytes)", (int)index, Gc_GameStateName(index), (unsigned)size);
    }
}

void* Gc_TraceCurGameState(s32* index, u32* frames) {
    *index = sCurGameState;
    *frames = sFrames;
    return sCurGameStatePtr;
}

void gc_game_crash_info(int* gameState, unsigned int* frames, int* scene, int* room, unsigned int* entrance) {
    PlayState* play = sCurGameStatePtr;

    *gameState = sCurGameState;
    *frames = sFrames;
    *scene = -1;
    *room = -1;
    *entrance = gSaveContext.save.entrance;
    if ((sCurGameState == GC_GAMESTATE_PLAY) && (play != NULL)) {
        *scene = play->sceneId;
        *room = play->roomCtx.curRoom.num;
    }
}

/** Log Play changes (scene, room, message boxes, cutscenes, form) as they happen; `report`: status line too. */
static void Gc_TracePlay(PlayState* play, s32 report) {
    Player* player = GET_PLAYER(play);
    s32 room = play->roomCtx.curRoom.num;
    s32 msgMode = play->msgCtx.msgMode;
    s32 textId = play->msgCtx.currentTextId;
    s32 csState = play->csCtx.state;
    s32 form = gSaveContext.save.playerForm;

    if (sPlayScene != play->sceneId) {
        sPlayScene = play->sceneId;
        gc_log("Play frame %u: scene %02X, room %d, day %d, time %04X, form %d", (unsigned)sFrames,
               (unsigned)play->sceneId, (int)room, (int)gSaveContext.save.day, (unsigned)gSaveContext.save.time,
               (int)form);
        sPlayRoom = room;
        sPlayForm = form;
    }
    if (sPlayRoom != room) {
        gc_log("Play frame %u: room %d -> %d", (unsigned)sFrames, (int)sPlayRoom, (int)room);
        sPlayRoom = room;
    }
    if (sPlayForm != form) {
        gc_log("Play frame %u: player form %d -> %d", (unsigned)sFrames, (int)sPlayForm, (int)form);
        sPlayForm = form;
    }
    if ((msgMode != MSGMODE_NONE) &&
        ((sPlayMsgMode == MSGMODE_NONE) || (sPlayMsgMode < 0) || (textId != sPlayTextId))) {
        gc_log("Play frame %u: message %04X (mode %d)", (unsigned)sFrames, (unsigned)textId, (int)msgMode);
    }
    sPlayMsgMode = msgMode;
    sPlayTextId = (msgMode != MSGMODE_NONE) ? textId : -1;
    if ((sPlayCsState != csState) && ((sPlayCsState <= CS_STATE_IDLE) || (csState == CS_STATE_IDLE))) {
        if (csState != CS_STATE_IDLE) {
            gc_log("Play frame %u: cutscene starts (script %d of %d)", (unsigned)sFrames,
                   (int)play->csCtx.scriptIndex, (int)play->csCtx.scriptListCount);
        } else if (sPlayCsState > CS_STATE_IDLE) {
            gc_log("Play frame %u: cutscene ends at cutscene frame %u", (unsigned)sFrames,
                   (unsigned)play->csCtx.curFrame);
        }
    }
    sPlayCsState = csState;

    // The status line only in unattended test builds: every log line is also written and synced to the SD card
    if (report && (player != NULL) && (GC_AUTOSTART != 0)) {
        Camera* cam = GET_ACTIVE_CAM(play);

        gc_log("Play: scene %02X room %d pos %d %d %d yaw %04X st %08X %08X msg %d/%d cs %d/%u trans %d time %04X "
               "cam %d eye %d %d %d at %d %d %d",
               (unsigned)play->sceneId, (int)room, (int)player->actor.world.pos.x, (int)player->actor.world.pos.y,
               (int)player->actor.world.pos.z, (unsigned)(u16)player->actor.shape.rot.y,
               (unsigned)player->stateFlags1, (unsigned)player->stateFlags2, (int)msgMode,
               (int)Message_GetState(&play->msgCtx), (int)csState, (unsigned)play->csCtx.curFrame,
               (int)play->transitionTrigger, (unsigned)gSaveContext.save.time, (int)cam->setting, (int)cam->eye.x,
               (int)cam->eye.y, (int)cam->eye.z, (int)cam->at.x, (int)cam->at.y, (int)cam->at.z);
    }
}

void Gc_TraceFrame(void) {
    OSTime now;
    u32 elapsedUs;
    s32 report = false;

    sFrames++;
    sFramesSinceReport++;

    now = osGetTime();
    elapsedUs = OS_CYCLES_TO_USEC(now - sReportTime);
    if (elapsedUs >= 1000000) {
        u32 fpsTimes10 = (sFramesSinceReport * 10000000ull) / elapsedUs;

        gc_log("%s frame %u, %u.%u fps, update %u us avg %u max", Gc_GameStateName(sCurGameState), (unsigned)sFrames,
               (unsigned)(fpsTimes10 / 10), (unsigned)(fpsTimes10 % 10),
               (unsigned)(sUpdateTotalUs / sFramesSinceReport), (unsigned)sUpdateMaxUs);
        sFramesSinceReport = 0;
        sReportTime = now;
        sUpdateTotalUs = 0;
        sUpdateMaxUs = 0;
        report = true;
    }

    if ((sCurGameState == GC_GAMESTATE_PLAY) && (sCurGameStatePtr != NULL)) {
        Gc_TracePlay(sCurGameStatePtr, report);
    }
    Gc_InputScriptFrame();
}

void Gc_TraceUpdateBegin(void) {
    sUpdateStart = osGetTime();
}

void Gc_TraceUpdateEnd(void) {
    u32 us = OS_CYCLES_TO_USEC(osGetTime() - sUpdateStart);

    sUpdateTotalUs += us;
    if (us > sUpdateMaxUs) {
        sUpdateMaxUs = us;
    }
}

void Gc_TraceGameStateEnd(void) {
    gc_log("gamestate %d %s end after %u frames", (int)sCurGameState, Gc_GameStateName(sCurGameState),
           (unsigned)sFrames);
    sCurGameStatePtr = NULL;
    Gc_InputScriptFrame();
}

void Gc_TraceCutsceneFlag(s16 flag) {
    if ((flag >= 0) && (flag < 32) && !(sCutsceneFlagsLogged & (1u << flag))) {
        sCutsceneFlagsLogged |= 1u << flag;
        gc_log("%s frame %u: cutscene flag %d set", Gc_GameStateName(sCurGameState), (unsigned)sFrames, (int)flag);
    }
}

s32 Gc_AutoStartMode(void) {
    return GC_AUTOSTART;
}

/**
 * GC_AUTOSTART=1 builds: during the title demo (Play before File Select was ever reached), tap Start for a
 * few frames every 2 seconds, so unattended runs reach File Select. Never presses anything afterwards.
 * (GC_AUTOSTART=2 builds do the same from the input script.)
 */
s32 Gc_AutoStartPressed(void) {
#if GC_AUTOSTART == 1
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

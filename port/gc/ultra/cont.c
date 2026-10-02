/**
 * libultra controller API (osContInit, osContStartQuery/GetQuery, osContStartReadData/GetReadData,
 * osContSetCh) on GameCube controllers.
 *
 * On N64 the Start* functions launch an SI DMA whose completion raises OS_EVENT_SI; the caller
 * (padmgr.c) then blocks on the SI event queue and calls the matching Get* function to decode the
 * PIF RAM. Any intermediate SI transfers a Start* or osContInit performs are waited for inside
 * the call, so each Start* leaves exactly one SI event for the caller and osContInit none. Here
 * the Start* functions poll the pads (gc_pad_read), keep a snapshot and post OS_EVENT_SI; the Get*
 * functions decode the snapshot.
 *
 * Port 0 is always reported as a standard controller with no error: when no GameCube pad is
 * connected (or it is not ready yet at boot) it reads as a neutral pad, so the game never sees
 * controller 1 unplugged. Ports 1-3 follow the GameCube pads. Connected ports report a controller
 * pak (CONT_CARD_ON), which motor.c presents as a rumble pak. A Voice Recognition Unit is never
 * reported, so padmgr.c never calls osVoice*. padmgr.c crashes on any errno other than 0, 0x4 and
 * 0x8; this file only produces 0 and CONT_NO_RESPONSE_ERROR (0x8).
 *
 * Button mapping (GameCube -> N64):
 *   A -> A, B -> B, Start -> Start, D-pad -> D-pad
 *   L (digital, or analog past the threshold) -> Z, R (same) -> R, Z -> L
 *   C-stick directions -> C buttons, X -> C-right, Y -> C-left
 *   main stick scaled by 0.8 and clamped to the N64 range
 */
#include "gc_ultra_internal.h"
#include "gc_game.h"
#include "stdbool.h"

#define GC_CONT_STICK_NUM 4 // main stick scale 4/5 = 0.8: GC about +-100 -> N64 about +-80
#define GC_CONT_STICK_DEN 5
#define GC_CONT_STICK_MAX 80          // N64 controllers reach about +-80 at the rim
#define GC_CONT_CSTICK_THRESHOLD 40   // C-stick deflection that presses a C button
#define GC_CONT_TRIGGER_THRESHOLD 0x60 // analog L/R value that counts as pressed

static s32 sContInitialized;
static u8 sContMaxControllers; // __osMaxControllers: 0 until osContInit, then osContSetCh
static gc_pad_t sContReadPads[MAXCONTROLLERS]; // snapshot taken by osContStartReadData
static u8 sContQueryPresent[MAXCONTROLLERS];   // snapshot taken by osContStartQuery

static s32 ContPadPresent(s32 chan, const gc_pad_t* pad) {
    return (chan == 0) || pad->connected;
}

static s8 ContScaleStick(s32 value) {
    value = value * GC_CONT_STICK_NUM / GC_CONT_STICK_DEN;
    if (value > GC_CONT_STICK_MAX) {
        value = GC_CONT_STICK_MAX;
    } else if (value < -GC_CONT_STICK_MAX) {
        value = -GC_CONT_STICK_MAX;
    }
    return value;
}

static u16 ContMapButtons(const gc_pad_t* pad) {
    u16 in = pad->buttons;
    u16 out = 0;

    if (in & GC_PAD_A) {
        out |= A_BUTTON;
    }
    if (in & GC_PAD_B) {
        out |= B_BUTTON;
    }
    if (in & GC_PAD_START) {
        out |= START_BUTTON;
    }
    if ((in & GC_PAD_L) || (pad->triggerL > GC_CONT_TRIGGER_THRESHOLD)) {
        out |= Z_TRIG;
    }
    if ((in & GC_PAD_R) || (pad->triggerR > GC_CONT_TRIGGER_THRESHOLD)) {
        out |= R_TRIG;
    }
    if (in & GC_PAD_Z) {
        out |= L_TRIG;
    }
    if (in & GC_PAD_UP) {
        out |= U_JPAD;
    }
    if (in & GC_PAD_DOWN) {
        out |= D_JPAD;
    }
    if (in & GC_PAD_LEFT) {
        out |= L_JPAD;
    }
    if (in & GC_PAD_RIGHT) {
        out |= R_JPAD;
    }
    if ((in & GC_PAD_X) || (pad->substickX > GC_CONT_CSTICK_THRESHOLD)) {
        out |= R_CBUTTONS;
    }
    if ((in & GC_PAD_Y) || (pad->substickX < -GC_CONT_CSTICK_THRESHOLD)) {
        out |= L_CBUTTONS;
    }
    if (pad->substickY > GC_CONT_CSTICK_THRESHOLD) {
        out |= U_CBUTTONS;
    }
    if (pad->substickY < -GC_CONT_CSTICK_THRESHOLD) {
        out |= D_CBUTTONS;
    }
    return out;
}

/** __osContGetInitData(): fill `data` for every polled port, return the bit pattern of present ones. */
static u8 ContFillStatus(const u8* present, OSContStatus* data) {
    s32 i;
    u8 bits = 0;

    for (i = 0; i < sContMaxControllers; i++, data++) {
        if (present[i]) {
            data->errno = 0;
            data->type = CONT_TYPE_NORMAL;
            data->status = CONT_CARD_ON;
            bits |= 1 << i;
        } else {
            // As on N64, type and status are left untouched for ports that do not answer
            data->errno = CONT_NO_RESPONSE_ERROR;
        }
    }
    return bits;
}

s32 osContInit(OSMesgQueue* mq, u8* bitpattern, OSContStatus* data) {
    gc_pad_t pads[MAXCONTROLLERS];
    u8 present[MAXCONTROLLERS];
    s32 i;

    gc_os_lock();
    if (sContInitialized) {
        gc_os_unlock();
        return 0;
    }
    sContInitialized = true;
    sContMaxControllers = MAXCONTROLLERS;
    gc_os_unlock();

    // libultra also waits until 0.5 s after boot here so the controllers have powered up; the
    // GameCube pads are initialised before the game starts, so that wait is skipped.
    gc_pad_read(pads);
    for (i = 0; i < MAXCONTROLLERS; i++) {
        present[i] = ContPadPresent(i, &pads[i]);
    }

    gc_os_lock();
    for (i = 0; i < MAXCONTROLLERS; i++) {
        sContQueryPresent[i] = present[i];
    }
    *bitpattern = ContFillStatus(present, data);
    gc_os_unlock();

    // osContInit consumes its own SI events on N64: nothing is posted for the caller
    return 0;
}

s32 osContStartQuery(OSMesgQueue* mq) {
    gc_pad_t pads[MAXCONTROLLERS];
    s32 i;

    gc_pad_read(pads);

    gc_os_lock();
    for (i = 0; i < MAXCONTROLLERS; i++) {
        sContQueryPresent[i] = ContPadPresent(i, &pads[i]);
    }
    gc_os_unlock();

    __gcPostEvent(OS_EVENT_SI);
    return 0;
}

void osContGetQuery(OSContStatus* data) {
    gc_os_lock();
    ContFillStatus(sContQueryPresent, data);
    gc_os_unlock();
}

s32 osContStartReadData(OSMesgQueue* mq) {
    gc_pad_t pads[MAXCONTROLLERS];
    s32 i;

    gc_pad_read(pads);

    gc_os_lock();
    for (i = 0; i < MAXCONTROLLERS; i++) {
        sContReadPads[i] = pads[i];
    }
    gc_os_unlock();

    __gcPostEvent(OS_EVENT_SI);
    return 0;
}

void osContGetReadData(OSContPad* data) {
    gc_pad_t pads[MAXCONTROLLERS];
    s32 count;
    s32 i;

    gc_os_lock();
    count = sContMaxControllers;
    for (i = 0; i < MAXCONTROLLERS; i++) {
        pads[i] = sContReadPads[i];
    }
    gc_os_unlock();

    for (i = 0; i < count; i++, data++) {
        if (!ContPadPresent(i, &pads[i])) {
            // As on N64, button and stick are left untouched when the port does not answer
            data->errno = CONT_NO_RESPONSE_ERROR;
            continue;
        }

        data->errno = 0;
        if (pads[i].connected) {
            data->button = ContMapButtons(&pads[i]);
            data->stick_x = ContScaleStick(pads[i].stickX);
            data->stick_y = ContScaleStick(pads[i].stickY);
        } else {
            // Port 0 with no pad plugged in: a controller that is not being touched
            data->button = 0;
            data->stick_x = 0;
            data->stick_y = 0;
        }

        // Unattended test builds (make GC_AUTOSTART=1) press Start on the title screen
        if ((i == 0) && Gc_AutoStartPressed()) {
            data->button |= START_BUTTON;
        }
    }
}

s32 osContSetCh(u8 ch) {
    gc_os_lock();
    sContMaxControllers = (ch > MAXCONTROLLERS) ? MAXCONTROLLERS : ch;
    gc_os_unlock();
    return 0;
}

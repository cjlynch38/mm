/**
 * Controllers: PAD_ScanPads and the PAD_* getters, one call per poll for all four ports.
 *
 * PAD_ScanPads returns a mask with bit (1 << chan) set for every port that answered this poll.
 * A port that does not answer is either unplugged (libogc then zeroes its state and resets the
 * port) or had a transient "not ready"/transfer error (libogc keeps its last state). The two
 * cannot be told apart from the mask, so a controller is only reported disconnected after it has
 * missed several polls in a row; until then the getters return its last good state, or zeros if
 * it was unplugged. This keeps the game from seeing one-frame disconnects.
 */
#include <gccore.h>
#include <string.h>
#include "gc_ogc.h"

/* Consecutive missed polls before a controller counts as disconnected */
#define PAD_DROPOUT_POLLS 8

/* PADStatus.button bits that are real buttons (0x0080 is the "use origin" flag) */
#define PAD_BUTTON_MASK 0x1F7F

static mutex_t sPadMutex = LWP_MUTEX_NULL;
static unsigned char sConnected[PAD_CHANMAX];
static unsigned char sMissed[PAD_CHANMAX];

void gc_ogc_pad_init(void) {
    PAD_Init();
    if (sPadMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sPadMutex, false) != 0) {
        gc_halt("gc_ogc_pad_init: cannot create the pad mutex");
    }
}

void gc_pad_read(gc_pad_t pads[4]) {
    unsigned int mask;
    int chan;

    LWP_MutexLock(sPadMutex);
    mask = PAD_ScanPads();
    for (chan = 0; chan < PAD_CHANMAX; chan++) {
        gc_pad_t* pad = &pads[chan];

        if (mask & (1u << chan)) {
            sConnected[chan] = 1;
            sMissed[chan] = 0;
        } else if (sConnected[chan] && ++sMissed[chan] >= PAD_DROPOUT_POLLS) {
            sConnected[chan] = 0;
        }

        memset(pad, 0, sizeof(*pad));
        pad->connected = sConnected[chan];
        if (pad->connected) {
            pad->buttons = PAD_ButtonsHeld(chan) & PAD_BUTTON_MASK;
            pad->stickX = PAD_StickX(chan);
            pad->stickY = PAD_StickY(chan);
            pad->substickX = PAD_SubStickX(chan);
            pad->substickY = PAD_SubStickY(chan);
            pad->triggerL = PAD_TriggerL(chan);
            pad->triggerR = PAD_TriggerR(chan);
        }
    }
    LWP_MutexUnlock(sPadMutex);
}

void gc_pad_rumble(int chan, int on) {
    if (chan < 0 || chan >= PAD_CHANMAX) {
        return;
    }
    // The N64 Rumble Pak motor coasts when switched off; PAD_MOTOR_STOP does the same, whereas
    // PAD_MOTOR_STOP_HARD brakes it.
    PAD_ControlMotor(chan, on ? PAD_MOTOR_RUMBLE : PAD_MOTOR_STOP);
}

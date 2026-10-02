/**
 * libultra OS events: osSetEventMesg and the shim-internal __gcPostEvent, which replaces the N64
 * interrupt handler's "send the registered message for this event" step. Also the Reset button,
 * which starts the N64 PRE-NMI sequence.
 */
#include "gc_os_core.h"

static __OSEventState sEventTable[OS_NUM_EVENTS];

// N64 __osShutdown: the reset button was pressed (PRE-NMI)
static s32 sShutdown;
// N64 __osPreNMI: a PRE-NMI message queue has been registered
static s32 sPreNMIRegistered;

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg m) {
    __OSEventState* es;

    if (e >= OS_NUM_EVENTS) {
        gc_log("osSetEventMesg: event %u out of range ignored", (unsigned int)e);
        return;
    }

    gc_os_lock();

    es = &sEventTable[e];
    es->messageQueue = mq;
    es->message = m;

    if (e == OS_EVENT_PRENMI) {
        // Reset already pressed before anyone listened: deliver it now, as libultra does
        if (sShutdown && !sPreNMIRegistered && (mq != NULL)) {
            __gcSendMesgLocked(mq, m, false);
        }
        sPreNMIRegistered = true;
    }

    gc_os_unlock();
}

void __gcPostEvent(OSEvent event) {
    __OSEventState* es;

    if (event >= OS_NUM_EVENTS) {
        return;
    }

    gc_os_lock();

    es = &sEventTable[event];
    // Like libultra's interrupt handler: NOBLOCK, the message is dropped if the queue is full
    if (es->messageQueue != NULL) {
        __gcSendMesgLocked(es->messageQueue, es->message, false);
    }

    gc_os_unlock();
}

void __gcOsResetPressed(void) {
    s32 first;

    gc_os_lock();
    first = !sShutdown;
    sShutdown = true;
    gc_os_unlock();

    if (first) {
        gc_log("Reset pressed: posting OS_EVENT_PRENMI");
        __gcPostEvent(OS_EVENT_PRENMI);
        // A second press hard-resets the console; make sure a recent save is on the card by then
        __gcFlashFlush();
    }
}

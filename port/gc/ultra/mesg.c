/**
 * libultra message queues: osCreateMesgQueue, osSendMesg, osJamMesg, osRecvMesg.
 *
 * The libultra ring-buffer algorithm runs in place on the game's OSMesgQueue fields (the game reads
 * validCount and msgCount directly and re-creates queues on the same memory). All queues share the
 * bridge's global OS lock and condition variable: a blocked thread waits on the condition and
 * re-checks its own queue after every broadcast, and every successful send, jam or receive
 * broadcasts. The highest-priority waiter therefore runs first and takes the message, as with
 * libultra's priority-ordered mtQueue/fullQueue lists, which are not used here.
 *
 * A zero-filled queue (never created, msgCount 0) is both empty and full: NOBLOCK sends and
 * receives fail and BLOCK ones wait forever, exactly as on the N64.
 */
#include "gc_os_core.h"

// mtQueue/fullQueue point here, as libultra's point at __osThreadTail. Nothing reads them.
static __OSThreadTail sEmptyThreadQueue = { NULL, -1 };

/** Store a message if there is room. Lock held. */
static s32 Mesg_PutLocked(OSMesgQueue* mq, OSMesg msg, s32 jam) {
    if (MQ_IS_FULL(mq)) {
        return -1;
    }

    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;

    gc_os_broadcast();
    return 0;
}

s32 __gcSendMesgLocked(OSMesgQueue* mq, OSMesg msg, s32 jam) {
    return Mesg_PutLocked(mq, msg, jam);
}

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msq, s32 count) {
    gc_os_lock();
    mq->mtQueue = (OSThread*)&sEmptyThreadQueue;
    mq->fullQueue = (OSThread*)&sEmptyThreadQueue;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msq;
    gc_os_unlock();
}

static s32 Mesg_Send(OSMesgQueue* mq, OSMesg msg, s32 flags, s32 jam) {
    gc_os_lock();
    __gcOsCheckpointLocked();

    while (MQ_IS_FULL(mq)) {
        // libultra blocks only for exactly OS_MESG_BLOCK when sending
        if (flags != OS_MESG_BLOCK) {
            gc_os_unlock();
            return -1;
        }
        __gcOsWaitLocked();
    }

    Mesg_PutLocked(mq, msg, jam);

    gc_os_unlock();
    return 0;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flags) {
    return Mesg_Send(mq, msg, flags, false);
}

s32 osJamMesg(OSMesgQueue* mq, OSMesg msg, s32 flag) {
    return Mesg_Send(mq, msg, flag, true);
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flags) {
    gc_os_lock();
    __gcOsCheckpointLocked();

    while (MQ_IS_EMPTY(mq)) {
        // ... and fails only for exactly OS_MESG_NOBLOCK when receiving
        if (flags == OS_MESG_NOBLOCK) {
            gc_os_unlock();
            return -1;
        }
        __gcOsWaitLocked();
    }

    if (msg != NULL) {
        *msg = mq->msg[mq->first];
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;

    gc_os_broadcast();

    gc_os_unlock();
    return 0;
}

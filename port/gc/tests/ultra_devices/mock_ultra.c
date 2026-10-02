/**
 * Minimal host versions of the libultra core the device files call (message queues, the event
 * table, osTvType, VI mode tables, the RCP register block), built with the decomp's headers.
 * Queues use libultra's ring-buffer algorithm; only NOBLOCK operations are supported, which is
 * all the device files use. Calls that the contract forbids while holding the OS lock fail the test.
 */
#include "gc_ultra_internal.h"
#include "stdbool.h"
#include "mock.h"

s32 osTvType = OS_TV_NTSC;

// Only the fields vi.c reads (comRegs.ctrl, comRegs.xScale) matter here
OSViMode osViModeNtscLan1 = { OS_VI_NTSC_LAN1, { 0x311E, 320, 0, 0, 0, 0, 0, 0x200, 0 } };
OSViMode osViModePalLan1 = { OS_VI_PAL_LAN1, { 0x311E, 320, 0, 0, 0, 0, 0, 0x200, 0 } };
OSViMode osViModeMpalLan1 = { OS_VI_MPAL_LAN1, { 0x311E, 320, 0, 0, 0, 0, 0, 0x200, 0 } };

static OSMesgQueue* sEventQueue[OS_NUM_EVENTS];
static OSMesg sEventMsg[OS_NUM_EVENTS];
static int sEventPosts[OS_NUM_EVENTS];

void osCreateMesgQueue(OSMesgQueue* mq, OSMesg* msg, s32 count) {
    mq->mtQueue = NULL;
    mq->fullQueue = NULL;
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msg;
}

static s32 MockSendLocked(OSMesgQueue* mq, OSMesg msg, s32 jam) {
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    if (jam) {
        mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
        mq->msg[mq->first] = msg;
    } else {
        mq->msg[(mq->first + mq->validCount) % mq->msgCount] = msg;
    }
    mq->validCount++;
    return 0;
}

s32 osSendMesg(OSMesgQueue* mq, OSMesg msg, s32 flags) {
    s32 ret;

    if (mock_os_lock_held()) {
        mock_fail("osSendMesg called with the OS lock held (would deadlock)");
        return -1;
    }
    gc_os_lock();
    ret = MockSendLocked(mq, msg, false);
    gc_os_unlock();
    if ((ret != 0) && (flags == OS_MESG_BLOCK)) {
        mock_fail("osSendMesg(BLOCK) on a full queue would block forever in this test");
    }
    return ret;
}

s32 osRecvMesg(OSMesgQueue* mq, OSMesg* msg, s32 flags) {
    s32 ret = -1;

    if (mock_os_lock_held()) {
        mock_fail("osRecvMesg called with the OS lock held (would deadlock)");
        return -1;
    }
    gc_os_lock();
    if (mq->validCount != 0) {
        if (msg != NULL) {
            *msg = mq->msg[mq->first];
        }
        mq->first = (mq->first + 1) % mq->msgCount;
        mq->validCount--;
        ret = 0;
    }
    gc_os_unlock();
    if ((ret != 0) && (flags == OS_MESG_BLOCK)) {
        mock_fail("osRecvMesg(BLOCK) on an empty queue would block forever in this test");
    }
    return ret;
}

s32 __gcSendMesgLocked(OSMesgQueue* mq, OSMesg msg, s32 jam) {
    if (!mock_os_lock_held()) {
        mock_fail("__gcSendMesgLocked called without the OS lock");
    }
    return MockSendLocked(mq, msg, jam);
}

void osSetEventMesg(OSEvent e, OSMesgQueue* mq, OSMesg m) {
    gc_os_lock();
    sEventQueue[e] = mq;
    sEventMsg[e] = m;
    gc_os_unlock();
}

void __gcPostEvent(OSEvent e) {
    OSMesgQueue* mq;
    OSMesg msg;

    if (mock_os_lock_held()) {
        mock_fail("__gcPostEvent(%lu) called with the OS lock held", (unsigned long)e);
        return;
    }
    gc_os_lock();
    sEventPosts[e]++;
    mq = sEventQueue[e];
    msg = sEventMsg[e];
    if (mq != NULL) {
        MockSendLocked(mq, msg, false);
    }
    gc_os_unlock();
}

int mock_event_posts(unsigned int event) {
    int count;

    gc_os_lock();
    count = sEventPosts[event];
    gc_os_unlock();
    return count;
}

// The RCP register block (port/gc/ultra/io.c in the real build): the RSP is always halted
u32 __gcIoRead(u32 addr) {
    return (addr == SP_STATUS_REG) ? SP_STATUS_HALT : 0;
}

void __gcIoWrite(u32 addr, u32 data) {
}

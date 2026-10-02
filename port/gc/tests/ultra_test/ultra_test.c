/**
 * Stand-alone test DOL for the libultra OS core of the GameCube shim (port/gc/ultra: thread.c,
 * mesg.c, event.c, intmask.c, time.c, timer.c) running on the real libogc bridge (port/gc/ogc).
 *
 * Compiled with the game's headers (Makefile.gc's port rule), like the shim itself. It boots
 * through gc_ultra_boot() with stand-ins for the game's bootproc() and the VI shim. Every failed
 * check is logged through gc_log (screen, Dolphin OSReport log); the last line is
 * "ULTRA_TEST DONE: <passed> passed, <failed> failed" or "ULTRA_TEST TIMEOUT ...".
 * The Makefile in this directory says how to build and run it.
 */
#include "ultra64.h"
#include "libc64/sleep.h"
#include "stdbool.h"
#include "../../ultra/gc_os_core.h"
#include "../../ogc/gc_ogc.h"

#define RUNNER_PRI 10
#define RUNNER_ID 100
#define TEST_TIMEOUT_SECONDS 60

/* -------------------------------------------------------------------------------------------- */
/* Test bookkeeping                                                                              */
/* -------------------------------------------------------------------------------------------- */

static s32 sPassed;
static s32 sFailed;
static const char* volatile sCurrentTest = "(none)";
static volatile s32 sDone;

static void Check(s32 ok, const char* expr, int line) {
    if (ok) {
        sPassed++;
    } else {
        sFailed++;
        gc_log("  FAIL %s:%d: %s", sCurrentTest, line, expr);
    }
}

#define CHECK(cond) Check((cond) != 0, #cond, __LINE__)
#define CHECK_EQ(a, b)                                                                                 \
    do {                                                                                               \
        s32 a_ = (s32)(a);                                                                             \
        s32 b_ = (s32)(b);                                                                             \
        if (a_ != b_) {                                                                                \
            gc_log("  FAIL %s:%d: %s == %s (%d != %d)", sCurrentTest, __LINE__, #a, #b, (int)a_, (int)b_); \
            sFailed++;                                                                                 \
        } else {                                                                                       \
            sPassed++;                                                                                 \
        }                                                                                              \
    } while (0)

static void Test_Begin(const char* name) {
    sCurrentTest = name;
    gc_log("== %s", name);
}

/* Event sequence written by the test threads, to check who ran when */
#define SEQ_MAX 64
static volatile s32 sSeq[SEQ_MAX];
static volatile s32 sSeqLen;

static void Seq_Reset(void) {
    sSeqLen = 0;
}

static void Seq_Add(s32 v) {
    if (sSeqLen < SEQ_MAX) {
        sSeq[sSeqLen] = v;
    }
    sSeqLen++;
}

static void Seq_Expect(const s32* expected, s32 count, int line) {
    s32 ok = (sSeqLen == count);
    s32 i;

    for (i = 0; ok && i < count; i++) {
        ok = (sSeq[i] == expected[i]);
    }
    if (ok) {
        sPassed++;
        return;
    }
    sFailed++;
    gc_log("  FAIL %s:%d: unexpected order (%d events)", sCurrentTest, line, (int)sSeqLen);
    for (i = 0; i < sSeqLen && i < SEQ_MAX; i++) {
        gc_log("    got[%d] = %d%s", (int)i, (int)sSeq[i],
               (i < count) ? ((sSeq[i] == expected[i]) ? "" : " (expected other)") : " (extra)");
    }
    for (; i < count; i++) {
        gc_log("    missing[%d] = %d", (int)i, (int)expected[i]);
    }
}

#define SEQ_EXPECT(...)                                         \
    do {                                                        \
        static const s32 expected_[] = { __VA_ARGS__ };         \
        Seq_Expect(expected_, ARRAY_COUNT_(expected_), __LINE__); \
    } while (0)

#define ARRAY_COUNT_(a) ((s32)(sizeof(a) / sizeof((a)[0])))
#define MSG(v) ((OSMesg)(intptr_t)(v))
#define VAL(m) ((s32)(intptr_t)(m))

static u64 Usec(OSTime cycles) {
    return OS_CYCLES_TO_USEC(cycles);
}

/* Busy-wait without involving the shim (works with interrupts disabled) */
static void SpinUsec(u32 usec) {
    u64 start = gc_time_ticks();
    u64 ticks = (u64)usec * GC_TB_HZ / 1000000;

    while (gc_time_ticks() - start < ticks) {}
}

/* Shared objects for the test threads */
static OSThread sThreads[8];
static OSMesgQueue sQ;
static OSMesg sQBuf[16];
static OSMesgQueue sReplyQ;
static OSMesg sReplyBuf[16];
static volatile s32 sCounter;

static void StartThread(s32 index, s32 id, void (*entry)(void*), void* arg, s32 pri) {
    osCreateThread(&sThreads[index], id, entry, arg, NULL, pri);
    osStartThread(&sThreads[index]);
}

/* -------------------------------------------------------------------------------------------- */
/* Message queues                                                                                */
/* -------------------------------------------------------------------------------------------- */

static void Test_QueueBasics(void) {
    OSMesgQueue q;
    OSMesg buf[3];
    OSMesg m = NULL;
    s32 i;
    s32 next;
    s32 expect;
    s32 ok;

    Test_Begin("queue basics (NOBLOCK, validCount, order, wrap-around)");

    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    CHECK(MQ_IS_EMPTY(&q) && !MQ_IS_FULL(&q));
    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_NOBLOCK), -1);

    CHECK_EQ(osSendMesg(&q, MSG(1), OS_MESG_NOBLOCK), 0);
    CHECK_EQ(q.validCount, 1);
    CHECK_EQ(osSendMesg(&q, MSG(2), OS_MESG_NOBLOCK), 0);
    CHECK_EQ(osSendMesg(&q, MSG(3), OS_MESG_NOBLOCK), 0);
    CHECK_EQ(q.validCount, 3);
    CHECK(MQ_IS_FULL(&q));
    CHECK_EQ(osSendMesg(&q, MSG(4), OS_MESG_NOBLOCK), -1);
    CHECK_EQ(osJamMesg(&q, MSG(4), OS_MESG_NOBLOCK), -1);
    CHECK_EQ(q.validCount, 3);

    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_NOBLOCK), 0);
    CHECK_EQ(VAL(m), 1);
    CHECK_EQ(q.validCount, 2);
    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_BLOCK), 0);
    CHECK_EQ(VAL(m), 2);
    CHECK_EQ(osRecvMesg(&q, NULL, OS_MESG_NOBLOCK), 0); // NULL: message dropped
    CHECK_EQ(q.validCount, 0);
    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_NOBLOCK), -1);

    // Many rounds so `first` wraps around the ring several times
    next = 100;
    expect = 100;
    ok = true;
    for (i = 0; i < 50; i++) {
        osSendMesg(&q, MSG(next++), OS_MESG_NOBLOCK);
        osSendMesg(&q, MSG(next++), OS_MESG_NOBLOCK);
        osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
        ok = ok && (VAL(m) == expect++);
        osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
        ok = ok && (VAL(m) == expect++);
    }
    CHECK(ok);
    CHECK_EQ(q.validCount, 0);

    // graph.c / audio drain idiom
    osSendMesg(&q, MSG(1), OS_MESG_NOBLOCK);
    osSendMesg(&q, MSG(2), OS_MESG_NOBLOCK);
    for (i = 0; (q.validCount != 0) && (i < 10); i++) {
        osRecvMesg(&q, NULL, OS_MESG_NOBLOCK);
    }
    CHECK_EQ(i, 2);
    CHECK_EQ(q.validCount, 0);

    // Re-creating a queue on the same memory resets it (audio load.c does this)
    osSendMesg(&q, MSG(1), OS_MESG_NOBLOCK);
    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    CHECK_EQ(q.validCount, 0);
    CHECK_EQ(q.first, 0);
}

static void Test_JamOrder(void) {
    OSMesgQueue q;
    OSMesg buf[4];
    OSMesg m = NULL;

    Test_Begin("jam ordering");

    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    osSendMesg(&q, MSG(1), OS_MESG_NOBLOCK);
    osSendMesg(&q, MSG(2), OS_MESG_NOBLOCK);
    CHECK_EQ(osJamMesg(&q, MSG(3), OS_MESG_NOBLOCK), 0);
    CHECK_EQ(osJamMesg(&q, MSG(4), OS_MESG_NOBLOCK), 0);
    CHECK_EQ(q.validCount, 4);
    CHECK_EQ(osJamMesg(&q, MSG(5), OS_MESG_NOBLOCK), -1);

    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 4);
    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 3);
    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 1);
    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 2);
    CHECK_EQ(q.validCount, 0);

    // Jam into an empty queue whose `first` is not 0
    osJamMesg(&q, MSG(7), OS_MESG_NOBLOCK);
    osSendMesg(&q, MSG(8), OS_MESG_NOBLOCK);
    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 7);
    osRecvMesg(&q, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 8);
}

static void Test_ZeroQueue(void) {
    static OSMesgQueue zq; // zero-filled, never created (like gAudioCtx.asyncLoadUnkMediumQueue)
    OSMesg m = MSG(42);

    Test_Begin("zero-filled queue");

    CHECK(MQ_IS_EMPTY(&zq));
    CHECK(MQ_IS_FULL(&zq));
    CHECK_EQ(osRecvMesg(&zq, &m, OS_MESG_NOBLOCK), -1);
    CHECK_EQ(VAL(m), 42);
    CHECK_EQ(osSendMesg(&zq, MSG(1), OS_MESG_NOBLOCK), -1);
    CHECK_EQ(osJamMesg(&zq, MSG(1), OS_MESG_NOBLOCK), -1);
    CHECK_EQ(zq.validCount, 0);
    CHECK_EQ(zq.first, 0);
}

/* -------------------------------------------------------------------------------------------- */
/* Blocking and priorities                                                                       */
/* -------------------------------------------------------------------------------------------- */

static void Recv_Entry(void* arg) {
    s32 tag = VAL(arg);
    OSMesg m = NULL;

    Seq_Add(tag);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    Seq_Add(tag + VAL(m));
}

static void RecvReply_Entry(void* arg) {
    s32 tag = VAL(arg);
    OSMesg m = NULL;

    Seq_Add(tag);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    Seq_Add(tag + VAL(m));
    osSendMesg(&sReplyQ, MSG(tag), OS_MESG_BLOCK);
}

static void Test_HigherReceiverPreempts(void) {
    Test_Begin("higher-priority receiver runs inside osSendMesg");

    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    Seq_Add(1);
    StartThread(0, 50, Recv_Entry, MSG(10), RUNNER_PRI + 10); // runs at once, blocks in recv
    Seq_Add(2);
    osSendMesg(&sQ, MSG(5), OS_MESG_BLOCK); // receiver must preempt right here
    Seq_Add(3);
    SEQ_EXPECT(1, 10, 2, 15, 3);
    CHECK_EQ(sQ.validCount, 0);
}

static void Test_LowerReceiverWaits(void) {
    OSMesg m = NULL;

    Test_Begin("lower-priority receiver runs only when the sender blocks");

    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    osCreateMesgQueue(&sReplyQ, sReplyBuf, 4);
    Seq_Add(1);
    StartThread(0, 51, RecvReply_Entry, MSG(10), RUNNER_PRI - 5);
    Seq_Add(2);
    osSendMesg(&sQ, MSG(5), OS_MESG_BLOCK);
    Seq_Add(3);
    CHECK_EQ(sQ.validCount, 1);
    osRecvMesg(&sReplyQ, &m, OS_MESG_BLOCK); // runner blocks: the receiver finally runs
    Seq_Add(4);
    SEQ_EXPECT(1, 2, 3, 10, 15, 4);
    CHECK_EQ(VAL(m), 10);
}

static void Test_TwoWaitersPriority(void) {
    Test_Begin("highest-priority waiter gets the message first");

    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    StartThread(0, 52, Recv_Entry, MSG(100), RUNNER_PRI + 5);
    StartThread(1, 53, Recv_Entry, MSG(200), RUNNER_PRI + 10);
    osSendMesg(&sQ, MSG(1), OS_MESG_BLOCK);
    Seq_Add(1);
    osSendMesg(&sQ, MSG(2), OS_MESG_BLOCK);
    Seq_Add(2);
    SEQ_EXPECT(100, 200, 201, 1, 102, 2);
}

static void Send_Entry(void* arg) {
    s32 tag = VAL(arg);

    Seq_Add(tag);
    osSendMesg(&sQ, MSG(tag + 1), OS_MESG_BLOCK);
    Seq_Add(tag + 1);
}

static void Jam_Entry(void* arg) {
    s32 tag = VAL(arg);

    Seq_Add(tag);
    osJamMesg(&sQ, MSG(tag + 1), OS_MESG_BLOCK);
    Seq_Add(tag + 1);
}

static void Test_FullQueueBlocks(void) {
    OSMesg m = NULL;

    Test_Begin("sender blocks on a full queue until a receive frees a slot");

    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 1);
    osSendMesg(&sQ, MSG(7), OS_MESG_NOBLOCK);
    Seq_Add(1);
    StartThread(0, 54, Send_Entry, MSG(10), RUNNER_PRI + 10); // blocks: queue full
    Seq_Add(2);
    CHECK_EQ(sQ.validCount, 1);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK); // sender completes before this returns
    Seq_Add(3);
    SEQ_EXPECT(1, 10, 2, 11, 3);
    CHECK_EQ(VAL(m), 7);
    CHECK_EQ(sQ.validCount, 1);
    osRecvMesg(&sQ, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 11);

    Test_Begin("osJamMesg blocks on a full queue, then jams at the front");

    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 2);
    osSendMesg(&sQ, MSG(1), OS_MESG_NOBLOCK);
    osSendMesg(&sQ, MSG(2), OS_MESG_NOBLOCK);
    StartThread(0, 55, Jam_Entry, MSG(20), RUNNER_PRI + 10);
    CHECK_EQ(sQ.validCount, 2);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    CHECK_EQ(VAL(m), 1);
    SEQ_EXPECT(20, 21);
    osRecvMesg(&sQ, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 21);
    osRecvMesg(&sQ, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 2);
}

/* -------------------------------------------------------------------------------------------- */
/* Thread API                                                                                    */
/* -------------------------------------------------------------------------------------------- */

static void Mark_Entry(void* arg) {
    Seq_Add(VAL(arg));
}

static void SendIndex_Entry(void* arg) {
    osSendMesg(&sReplyQ, arg, OS_MESG_BLOCK);
}

static void Test_ThreadApi(void) {
    OSThread* it;
    s32 found = false;
    s32 n;

    Test_Begin("osGetThreadId/Pri, osSetThreadPri preemption, active list");

    CHECK_EQ(osGetThreadId(NULL), RUNNER_ID);
    CHECK_EQ(osGetThreadPri(NULL), RUNNER_PRI);
    CHECK(__gcGetCurrentThread() != NULL);
    CHECK_EQ(__gcGetCurrentThread()->id, RUNNER_ID);

    // Lowering the caller below a ready thread lets that thread run before osSetThreadPri returns
    Seq_Reset();
    StartThread(0, 60, Mark_Entry, MSG(50), RUNNER_PRI - 5);
    Seq_Add(1);
    osSetThreadPri(NULL, RUNNER_PRI - 7);
    Seq_Add(2);
    osSetThreadPri(NULL, RUNNER_PRI);
    CHECK_EQ(osGetThreadPri(NULL), RUNNER_PRI);
    SEQ_EXPECT(1, 50, 2);

    // Raising a ready thread above the caller makes it run at once
    Seq_Reset();
    StartThread(0, 61, Mark_Entry, MSG(60), RUNNER_PRI - 5);
    Seq_Add(1);
    CHECK_EQ(osGetThreadPri(&sThreads[0]), RUNNER_PRI - 5);
    osSetThreadPri(&sThreads[0], RUNNER_PRI + 5);
    Seq_Add(2);
    SEQ_EXPECT(1, 60, 2);

    // osGetThreadId/osGetThreadPri on another thread
    osCreateThread(&sThreads[1], 62, Mark_Entry, MSG(0), NULL, 33);
    CHECK_EQ(osGetThreadId(&sThreads[1]), 62);
    CHECK_EQ(osGetThreadPri(&sThreads[1]), 33);

    // The active list holds created threads and ends at the priority -1 sentinel
    n = 0;
    for (it = __osGetActiveQueue(); (it->priority != -1) && (n < 100); it = it->tlnext, n++) {
        if (it == &sThreads[1]) {
            found = true;
        }
    }
    CHECK(n < 100);
    CHECK(found);
    osDestroyThread(&sThreads[1]); // never started
    found = false;
    for (it = __osGetActiveQueue(); (it->priority != -1) && (n < 200); it = it->tlnext, n++) {
        if (it == &sThreads[1]) {
            found = true;
        }
    }
    CHECK(!found);

    CHECK_EQ(__gcMapPriority(0), GC_PRIO_IDLE);
    CHECK_EQ(__gcMapPriority(1), 1);
    CHECK_EQ(__gcMapPriority(18), 18);
    CHECK_EQ(__gcMapPriority(OS_PRIORITY_APPMAX), GC_PRIO_GAME_MAX);
    CHECK_EQ(__gcMapPriority(OS_PRIORITY_VIMGR), GC_PRIO_GAME_MAX);
}

static void Test_ThreadReuse(void) {
    OSMesg m = NULL;
    s32 i;
    s32 ok = true;

    Test_Begin("60 short-lived threads (worker reuse), re-created OSThread");

    osCreateMesgQueue(&sReplyQ, sReplyBuf, 4);
    for (i = 0; i < 60; i++) {
        // Higher priority: each runs to completion inside osStartThread
        StartThread(i % 2, 70 + i, SendIndex_Entry, MSG(i), RUNNER_PRI + 1 + (i % 3));
        osRecvMesg(&sReplyQ, &m, OS_MESG_BLOCK);
        ok = ok && (VAL(m) == i);
        // sys_flashrom destroys the thread after its entry returned
        osDestroyThread(&sThreads[i % 2]);
    }
    CHECK(ok);

    // Lower priority: the entry runs when the runner blocks
    for (i = 0; i < 10; i++) {
        StartThread(0, 130 + i, SendIndex_Entry, MSG(1000 + i), RUNNER_PRI - 1);
        osRecvMesg(&sReplyQ, &m, OS_MESG_BLOCK);
        ok = ok && (VAL(m) == 1000 + i);
    }
    CHECK(ok);

    // A finished thread is not restarted (the sleep lets the last one return from its entry)
    msleep(1);
    osStartThread(&sThreads[0]);
    CHECK_EQ(osRecvMesg(&sReplyQ, &m, OS_MESG_NOBLOCK), -1);
}

static void SelfDestroy_Entry(void* arg) {
    Seq_Add(10);
    osDestroyThread(NULL);
    Seq_Add(11); // never reached
}

static void Counter_Entry(void* arg) {
    s32 i;
    OSMesg m = NULL;

    for (i = 0; i < 3; i++) {
        osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
        sCounter += VAL(m);
    }
}

static void Test_DestroyStop(void) {
    Test_Begin("osDestroyThread on a blocked thread, on itself; osStopThread/osStartThread");

    // A destroyed receiver must never take the message
    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    StartThread(0, 80, Recv_Entry, MSG(10), RUNNER_PRI + 10); // blocks in recv
    osDestroyThread(&sThreads[0]);
    osSendMesg(&sQ, MSG(1), OS_MESG_NOBLOCK);
    msleep(5);
    CHECK_EQ(sQ.validCount, 1);
    SEQ_EXPECT(10);

    // A thread destroying itself stops there
    Seq_Reset();
    StartThread(1, 81, SelfDestroy_Entry, NULL, RUNNER_PRI + 10);
    msleep(2);
    SEQ_EXPECT(10);

    // A stopped receiver does not run until restarted
    osCreateMesgQueue(&sQ, sQBuf, 4);
    sCounter = 0;
    StartThread(2, 82, Counter_Entry, NULL, RUNNER_PRI + 10); // blocks in recv
    osStopThread(&sThreads[2]);
    osSendMesg(&sQ, MSG(1), OS_MESG_NOBLOCK);
    msleep(2);
    CHECK_EQ(sCounter, 0);
    CHECK_EQ(sQ.validCount, 1);
    osStartThread(&sThreads[2]); // preempts the runner and takes the message
    CHECK_EQ(sCounter, 1);
    CHECK_EQ(sQ.validCount, 0);
    osSendMesg(&sQ, MSG(10), OS_MESG_NOBLOCK);
    CHECK_EQ(sCounter, 11);
    osSendMesg(&sQ, MSG(100), OS_MESG_NOBLOCK);
    CHECK_EQ(sCounter, 111);
}

/* -------------------------------------------------------------------------------------------- */
/* Time and timers                                                                               */
/* -------------------------------------------------------------------------------------------- */

static void Test_TimeRate(void) {
    u64 k0;
    u64 k1;
    OSTime t0;
    OSTime t1;
    u64 expect;
    u64 diff;
    u64 usec;
    u32 count;
    u32 low;

    Test_Begin("osGetTime rate (46.875 MHz from the 40.5 MHz timebase)");

    k0 = gc_time_ticks();
    t0 = osGetTime();
    msleep(100);
    t1 = osGetTime();
    k1 = gc_time_ticks();

    expect = (k1 - k0) * 125 / 108;
    diff = (expect > (t1 - t0)) ? expect - (t1 - t0) : (t1 - t0) - expect;
    usec = Usec(t1 - t0);
    gc_log("  100 ms sleep: osGetTime %u us, timebase %u us, |diff| %u cycles", (unsigned int)usec,
           (unsigned int)((k1 - k0) * 1000000 / GC_TB_HZ), (unsigned int)diff);
    CHECK(diff < 2000); // the two reads are a few instructions apart
    CHECK(usec >= 100000 && usec < 115000);
    CHECK(t1 > t0);
    count = osGetCount();
    low = (u32)osGetTime();
    CHECK(low - count < 100000u); // osGetCount is the low word of osGetTime

    CHECK_EQ((s32)__gcCyclesToTicks(125), 108);
    CHECK_EQ((s32)__gcCyclesToTicks(126), 109); // rounded up
}

static void Test_Timers(void) {
    OSMesgQueue q;
    OSMesg buf[16];
    OSMesg m = NULL;
    OSTimer t;
    OSTimer many[16];
    OSTime start;
    u64 us;
    s32 i;
    s32 ok;
    static const s32 sOrder[16] = { 9, 3, 14, 0, 7, 12, 5, 1, 15, 10, 2, 8, 13, 4, 11, 6 };

    Test_Begin("one-shot timer");
    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    start = osGetTime();
    CHECK_EQ(osSetTimer(&t, OS_USEC_TO_CYCLES(20000), 0, &q, MSG(0x1234)), 0);
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    us = Usec(osGetTime() - start);
    gc_log("  20 ms one-shot fired after %u us", (unsigned int)us);
    CHECK_EQ(VAL(m), 0x1234);
    CHECK(us >= 20000 && us < 26000);
    CHECK_EQ(osStopTimer(&t), -1); // already fired and unlinked
    msleep(30);
    CHECK_EQ(q.validCount, 0);

    Test_Begin("interval timer");
    start = osGetTime();
    osSetTimer(&t, OS_USEC_TO_CYCLES(10000), OS_USEC_TO_CYCLES(10000), &q, MSG(7));
    ok = true;
    for (i = 0; i < 5; i++) {
        osRecvMesg(&q, &m, OS_MESG_BLOCK);
        ok = ok && (VAL(m) == 7);
    }
    us = Usec(osGetTime() - start);
    gc_log("  5 x 10 ms interval took %u us", (unsigned int)us);
    CHECK(ok);
    CHECK(us >= 50000 && us < 56000);
    CHECK_EQ(osStopTimer(&t), 0);
    msleep(25);
    CHECK_EQ(q.validCount, 0);

    Test_Begin("countdown 0 means one interval");
    start = osGetTime();
    osSetTimer(&t, 0, OS_USEC_TO_CYCLES(5000), &q, MSG(8));
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    us = Usec(osGetTime() - start);
    CHECK(us >= 5000 && us < 9000);
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    CHECK_EQ(osStopTimer(&t), 0);
    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));

    Test_Begin("stop a pending timer, stop an unset timer, re-set an active timer");
    osSetTimer(&t, OS_USEC_TO_CYCLES(10000), 0, &q, MSG(1));
    CHECK_EQ(osStopTimer(&t), 0);
    CHECK_EQ(osStopTimer(&t), -1);
    {
        OSTimer garbage;
        s32 j;

        for (j = 0; j < (s32)sizeof(garbage); j++) {
            ((u8*)&garbage)[j] = 0xA5;
        }
        CHECK_EQ(osStopTimer(&garbage), -1);
    }
    msleep(20);
    CHECK_EQ(q.validCount, 0);
    start = osGetTime();
    osSetTimer(&t, OS_USEC_TO_CYCLES(50000), 0, &q, MSG(1));
    osSetTimer(&t, OS_USEC_TO_CYCLES(5000), 0, &q, MSG(2)); // replaces the first
    osRecvMesg(&q, &m, OS_MESG_BLOCK);
    us = Usec(osGetTime() - start);
    CHECK_EQ(VAL(m), 2);
    CHECK(us < 9000);
    msleep(60);
    CHECK_EQ(q.validCount, 0);

    Test_Begin("16 concurrent timers fire in expiry order");
    for (i = 0; i < 16; i++) {
        s32 k = sOrder[i];

        osSetTimer(&many[k], OS_USEC_TO_CYCLES(2000 + 1000 * k), 0, &q, MSG(k));
    }
    ok = true;
    for (i = 0; i < 16; i++) {
        osRecvMesg(&q, &m, OS_MESG_BLOCK);
        if (VAL(m) != i) {
            gc_log("  timer %d fired at position %d", (int)VAL(m), (int)i);
            ok = false;
        }
    }
    CHECK(ok);

    Test_Begin("10 us timers (usleep(10), the yaz0.c lock spin)");
    start = osGetTime();
    for (i = 0; i < 1000; i++) {
        usleep(10);
    }
    us = Usec(osGetTime() - start);
    gc_log("  1000 x usleep(10) took %u us", (unsigned int)us);
    CHECK(us >= 10000);
    CHECK(us < 1000000);
    start = osGetTime();
    for (i = 0; i < 100; i++) {
        csleep(0); // countdown 0, interval 0: fires immediately
    }
    us = Usec(osGetTime() - start);
    gc_log("  100 x csleep(0) took %u us", (unsigned int)us);
    CHECK(us < 100000);
}

/* -------------------------------------------------------------------------------------------- */
/* Events and interrupt mask                                                                     */
/* -------------------------------------------------------------------------------------------- */

static gc_sem_t sServiceSem;
static volatile OSEvent sServiceEvent;

/* A shim-style service thread (not a game thread) posting events, like the VI thread */
static void Service_Entry(void* arg) {
    for (;;) {
        gc_sem_wait(sServiceSem);
        __gcPostEvent(sServiceEvent);
    }
}

static void EventWaiter_Entry(void* arg) {
    OSMesg m = NULL;

    Seq_Add(10);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    Seq_Add(VAL(m));
}

static void Test_Events(void) {
    OSMesgQueue q;
    OSMesg buf[1];
    OSMesg m = NULL;
    gc_thread_t service;

    Test_Begin("events: osSetEventMesg + __gcPostEvent");

    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    osSetEventMesg(OS_EVENT_SI, &q, MSG(0x55));
    __gcPostEvent(OS_EVENT_SI);
    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_NOBLOCK), 0);
    CHECK_EQ(VAL(m), 0x55);

    // A full queue drops the event instead of blocking the poster
    __gcPostEvent(OS_EVENT_SI);
    __gcPostEvent(OS_EVENT_SI);
    CHECK_EQ(q.validCount, 1);
    osRecvMesg(&q, NULL, OS_MESG_NOBLOCK);

    // Unregistered and out-of-range events are ignored
    __gcPostEvent(OS_EVENT_CART);
    __gcPostEvent(OS_NUM_EVENTS + 3);
    osSetEventMesg(OS_EVENT_SI, NULL, NULL);
    __gcPostEvent(OS_EVENT_SI);
    CHECK_EQ(q.validCount, 0);

    // From a service thread, waking a blocked higher-priority game thread
    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    osSetEventMesg(OS_EVENT_DP, &sQ, MSG(668));
    CHECK_EQ(gc_sem_create(&sServiceSem, 0), 0);
    sServiceEvent = OS_EVENT_DP;
    CHECK_EQ(gc_thread_create(&service, Service_Entry, NULL, 0x2000, GC_PRIO_SERVICE_VI - 2), 0);
    StartThread(0, 90, EventWaiter_Entry, NULL, RUNNER_PRI + 10);
    Seq_Add(1);
    gc_sem_post(sServiceSem); // service posts, waiter runs, all before this returns
    Seq_Add(2);
    SEQ_EXPECT(10, 1, 668, 2);
    CHECK(__gcGetCurrentThread() != NULL);

    // Reset: PRE-NMI is delivered once, also to a queue registered afterwards
    Test_Begin("reset -> OS_EVENT_PRENMI");
    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    osSetEventMesg(OS_EVENT_PRENMI, &q, MSG(669));
    __gcOsResetPressed();
    CHECK_EQ(osRecvMesg(&q, &m, OS_MESG_NOBLOCK), 0);
    CHECK_EQ(VAL(m), 669);
    __gcOsResetPressed();
    CHECK_EQ(q.validCount, 0);
}

static void Test_IntMask(void) {
    OSMesgQueue q;
    OSMesg buf[2];
    OSTimer t;
    OSIntMask outer;
    OSIntMask inner;
    s32 countMasked;
    s32 countAfter;

    Test_Begin("osSetIntMask nesting and return values");

    CHECK_EQ(osGetIntMask(), OS_IM_ALL);
    outer = osSetIntMask(OS_IM_NONE);
    CHECK_EQ(outer, OS_IM_ALL);
    inner = osSetIntMask(OS_IM_NONE);
    CHECK_EQ(inner, OS_IM_NONE);
    CHECK_EQ(osGetIntMask(), OS_IM_NONE);
    CHECK_EQ(osSetIntMask(inner), OS_IM_NONE);
    CHECK_EQ(osGetIntMask(), OS_IM_NONE);
    CHECK_EQ(osSetIntMask(outer), OS_IM_NONE);
    CHECK_EQ(osGetIntMask(), OS_IM_ALL);

    Test_Begin("OS_IM_NONE blocks preemption by the timer thread");
    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    osSetTimer(&t, OS_USEC_TO_CYCLES(1000), 0, &q, MSG(1));
    outer = osSetIntMask(OS_IM_NONE);
    SpinUsec(3000);
    countMasked = q.validCount;
    osSetIntMask(outer);
    countAfter = q.validCount; // the timer thread runs as soon as interrupts are back on
    CHECK_EQ(countMasked, 0);
    CHECK_EQ(countAfter, 1);
    osRecvMesg(&q, NULL, OS_MESG_BLOCK);
    CHECK_EQ(osGetIntMask(), OS_IM_ALL);
}

/* -------------------------------------------------------------------------------------------- */
/* Stress: producers and consumers at mixed priorities through a small queue                     */
/* -------------------------------------------------------------------------------------------- */

#define STRESS_PRODUCERS 3
#define STRESS_CONSUMERS 2
#define STRESS_COUNT 3000

static OSMesgQueue sStressQ;
static OSMesg sStressBuf[4];
static OSMesgQueue sStressDoneQ;
static OSMesg sStressDoneBuf[8];
static volatile s32 sStressRecv[STRESS_CONSUMERS];
static volatile u32 sStressSum[STRESS_CONSUMERS];
static volatile s32 sStressOrderOk;

static void Producer_Entry(void* arg) {
    s32 id = VAL(arg);
    s32 i;

    for (i = 1; i <= STRESS_COUNT; i++) {
        osSendMesg(&sStressQ, MSG((id << 16) | i), OS_MESG_BLOCK);
        if ((i % 500) == 0) {
            osYieldThread();
        }
    }
    osSendMesg(&sStressDoneQ, MSG(id), OS_MESG_BLOCK);
}

static void Consumer_Entry(void* arg) {
    s32 id = VAL(arg);
    s32 last[STRESS_PRODUCERS] = { 0 };
    OSMesg m = NULL;

    for (;;) {
        s32 v;

        osRecvMesg(&sStressQ, &m, OS_MESG_BLOCK);
        v = VAL(m);
        if (v == -1) {
            break;
        }
        if ((v & 0xFFFF) <= last[v >> 16]) {
            sStressOrderOk = false;
        }
        last[v >> 16] = v & 0xFFFF;
        sStressRecv[id]++;
        sStressSum[id] += v & 0xFFFF;
    }
    osSendMesg(&sStressDoneQ, MSG(100 + id), OS_MESG_BLOCK);
}

static void Test_Stress(void) {
    static const s32 sProducerPri[STRESS_PRODUCERS] = { RUNNER_PRI - 4, RUNNER_PRI - 2, RUNNER_PRI + 2 };
    static const s32 sConsumerPri[STRESS_CONSUMERS] = { RUNNER_PRI - 3, RUNNER_PRI + 1 };
    OSTime start;
    s32 i;
    s32 total = 0;
    u32 sum = 0;

    Test_Begin("stress: 3 producers, 2 consumers, queue of 4");

    osCreateMesgQueue(&sStressQ, sStressBuf, ARRAY_COUNT_(sStressBuf));
    osCreateMesgQueue(&sStressDoneQ, sStressDoneBuf, ARRAY_COUNT_(sStressDoneBuf));
    sStressOrderOk = true;
    start = osGetTime();
    for (i = 0; i < STRESS_CONSUMERS; i++) {
        sStressRecv[i] = 0;
        sStressSum[i] = 0;
        StartThread(3 + i, 200 + i, Consumer_Entry, MSG(i), sConsumerPri[i]);
    }
    for (i = 0; i < STRESS_PRODUCERS; i++) {
        StartThread(5 + i, 210 + i, Producer_Entry, MSG(i), sProducerPri[i]);
    }
    for (i = 0; i < STRESS_PRODUCERS; i++) {
        osRecvMesg(&sStressDoneQ, NULL, OS_MESG_BLOCK);
    }
    for (i = 0; i < STRESS_CONSUMERS; i++) {
        osSendMesg(&sStressQ, MSG(-1), OS_MESG_BLOCK);
    }
    for (i = 0; i < STRESS_CONSUMERS; i++) {
        osRecvMesg(&sStressDoneQ, NULL, OS_MESG_BLOCK);
    }
    for (i = 0; i < STRESS_CONSUMERS; i++) {
        total += sStressRecv[i];
        sum += sStressSum[i];
    }
    gc_log("  %d messages in %u us (consumer split %d/%d)", (int)total, (unsigned int)Usec(osGetTime() - start),
           (int)sStressRecv[0], (int)sStressRecv[1]);
    CHECK_EQ(total, STRESS_PRODUCERS * STRESS_COUNT);
    CHECK(sum == (u32)STRESS_PRODUCERS * (STRESS_COUNT * (STRESS_COUNT + 1) / 2));
    CHECK(sStressOrderOk);
}

/* -------------------------------------------------------------------------------------------- */
/* Edge cases added by review: stop/destroy of blocked senders, re-creation, priorities,         */
/* concurrent sleepers, masked blocking, far-future timers                                        */
/* -------------------------------------------------------------------------------------------- */

static void SelfStop_Entry(void* arg) {
    Seq_Add(10);
    osStopThread(NULL);
    Seq_Add(11);
}

static void Test_StopDestroySenders(void) {
    OSMesg m = NULL;

    Test_Begin("destroyed/stopped blocked senders; osStopThread(NULL)");

    // A sender destroyed while blocked on a full queue never delivers
    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 1);
    osSendMesg(&sQ, MSG(7), OS_MESG_NOBLOCK);
    StartThread(0, 300, Send_Entry, MSG(10), RUNNER_PRI + 10); // blocks: queue full
    osDestroyThread(&sThreads[0]);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    CHECK_EQ(VAL(m), 7);
    msleep(2);
    CHECK_EQ(sQ.validCount, 0);
    SEQ_EXPECT(10);

    // A stopped blocked sender leaves a freed slot alone and sends only after osStartThread
    Seq_Reset();
    osSendMesg(&sQ, MSG(7), OS_MESG_NOBLOCK);
    StartThread(1, 301, Send_Entry, MSG(20), RUNNER_PRI + 10); // blocks: queue full
    osStopThread(&sThreads[1]);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK);
    CHECK_EQ(VAL(m), 7);
    msleep(2);
    CHECK_EQ(sQ.validCount, 0);
    CHECK_EQ(osSendMesg(&sQ, MSG(30), OS_MESG_NOBLOCK), 0); // the slot is still free for others
    osStartThread(&sThreads[1]);                            // queue full again: it keeps waiting
    CHECK_EQ(sQ.validCount, 1);
    osRecvMesg(&sQ, &m, OS_MESG_BLOCK); // the restarted sender fills the slot before this returns
    CHECK_EQ(VAL(m), 30);
    CHECK_EQ(sQ.validCount, 1);
    osRecvMesg(&sQ, &m, OS_MESG_NOBLOCK);
    CHECK_EQ(VAL(m), 21);
    SEQ_EXPECT(20, 21);

    // A thread stopping itself resumes (preempting the caller) when restarted
    Seq_Reset();
    StartThread(2, 302, SelfStop_Entry, NULL, RUNNER_PRI + 5);
    Seq_Add(1);
    osStartThread(&sThreads[2]);
    Seq_Add(2);
    SEQ_EXPECT(10, 1, 11, 2);
}

static void Test_RecreateAndPriorities(void) {
    Test_Begin("re-created blocked thread, raised waiter, pooled worker priority");

    // Re-creating an OSThread whose thread is blocked: the old instance never consumes
    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    StartThread(3, 303, Recv_Entry, MSG(10), RUNNER_PRI + 10); // blocks in recv
    StartThread(3, 304, Recv_Entry, MSG(20), RUNNER_PRI + 10); // same OSThread, new instance
    osSendMesg(&sQ, MSG(1), OS_MESG_BLOCK);
    msleep(2);
    SEQ_EXPECT(10, 20, 21);
    CHECK_EQ(sQ.validCount, 0);
    CHECK_EQ(osGetThreadId(&sThreads[3]), 304);

    // Raising a blocked waiter above another one makes it take the next message
    Seq_Reset();
    osCreateMesgQueue(&sQ, sQBuf, 4);
    StartThread(0, 305, Recv_Entry, MSG(100), RUNNER_PRI + 10);
    StartThread(1, 306, Recv_Entry, MSG(200), RUNNER_PRI + 5);
    osSetThreadPri(&sThreads[1], RUNNER_PRI + 15);
    CHECK_EQ(osGetThreadPri(&sThreads[1]), RUNNER_PRI + 15);
    osSendMesg(&sQ, MSG(1), OS_MESG_BLOCK);
    osSendMesg(&sQ, MSG(2), OS_MESG_BLOCK);
    SEQ_EXPECT(100, 200, 201, 102);

    // A pooled worker that last ran at a high priority takes the lower priority of its next job
    Seq_Reset();
    StartThread(4, 307, Mark_Entry, MSG(1), RUNNER_PRI + 5); // runs at once
    msleep(1);
    StartThread(4, 308, Mark_Entry, MSG(2), RUNNER_PRI - 5); // must wait until the runner blocks
    Seq_Add(3);
    msleep(1);
    SEQ_EXPECT(1, 3, 2);
}

/* yaz0.c-style busy flag guarded by a usleep(10) spin, used by threads at different priorities */
static volatile s32 sSpinBusy;

static void Spin_Entry(void* arg) {
    s32 i;

    for (i = 0; i < 100; i++) {
        while (sSpinBusy) {
            usleep(10);
        }
        sSpinBusy = true;
        usleep(30);
        sSpinBusy = false;
        usleep(10);
    }
    osSendMesg(&sReplyQ, arg, OS_MESG_BLOCK);
}

static void Test_ConcurrentSleepers(void) {
    OSTime start;
    OSMesg m = NULL;
    u64 us;
    s32 sum = 0;
    s32 i;

    Test_Begin("4 threads spinning on usleep(10) at mixed priorities (yaz0.c pattern)");

    osCreateMesgQueue(&sReplyQ, sReplyBuf, 8);
    sSpinBusy = false;
    start = osGetTime();
    StartThread(0, 310, Spin_Entry, MSG(1), RUNNER_PRI + 7);
    StartThread(1, 311, Spin_Entry, MSG(2), RUNNER_PRI + 2);
    StartThread(2, 312, Spin_Entry, MSG(4), RUNNER_PRI + 1);
    StartThread(3, 313, Spin_Entry, MSG(8), RUNNER_PRI - 1);
    for (i = 0; i < 4; i++) {
        osRecvMesg(&sReplyQ, &m, OS_MESG_BLOCK);
        sum += VAL(m);
    }
    us = Usec(osGetTime() - start);
    gc_log("  4 x 100 rounds took %u us", (unsigned int)us);
    CHECK_EQ(sum, 15);
    CHECK(us < 2000000);
}

static void Test_MaskedBlockingAndFarTimers(void) {
    OSMesgQueue q;
    OSMesg buf[4];
    OSTimer t;
    OSTimer t2;
    OSIntMask prev;
    s32 countMasked;

    Test_Begin("blocking with OS_IM_NONE lets others run; the mask survives the wait");

    osCreateMesgQueue(&q, buf, ARRAY_COUNT_(buf));
    prev = osSetIntMask(OS_IM_NONE);
    osSetTimer(&t, OS_USEC_TO_CYCLES(2000), 0, &q, MSG(1));
    osRecvMesg(&q, NULL, OS_MESG_BLOCK); // the timer thread must run while this thread waits
    CHECK_EQ(osGetIntMask(), OS_IM_NONE);
    osSetTimer(&t, OS_USEC_TO_CYCLES(1000), 0, &q, MSG(2));
    SpinUsec(3000);
    countMasked = q.validCount; // still masked after the wait: not preempted by the timer thread
    osSetIntMask(prev);
    CHECK_EQ(countMasked, 0);
    CHECK_EQ(q.validCount, 1);
    osRecvMesg(&q, NULL, OS_MESG_BLOCK);
    CHECK_EQ(osGetIntMask(), OS_IM_ALL);

    Test_Begin("far-future timers never fire early (no 64-bit wrap of the deadline)");
    osSetTimer(&t, 0xFFFFFFFFFFFFFFFFULL, 0, &q, MSG(3));
    osSetTimer(&t2, 0x8000000000000000ULL, 0xFFFFFFFFFFFFFFF0ULL, &q, MSG(4));
    msleep(20);
    CHECK_EQ(q.validCount, 0);
    CHECK_EQ(osStopTimer(&t), 0);
    CHECK_EQ(osStopTimer(&t2), 0);
}

/* -------------------------------------------------------------------------------------------- */
/* Mini frame loop: the game's M2 message protocol under load                                    */
/*   VI service thread (60 Hz) -> OS_EVENT_VI -> IrqMgr (18) -> NOBLOCK retrace to Sched (16),   */
/*   PadMgr (15), Main (12), AudioMgr (11); Graph (9) posts a task + NOTIFY and waits for it      */
/*   with a 3 s watchdog timer (graph.c); AudioMgr waits for its task with a 32 ms watchdog       */
/*   (audio_thread_manager.c); PadMgr takes a 1-slot serial lock queue and waits for OS_EVENT_SI. */
/* -------------------------------------------------------------------------------------------- */

#define LOOP_FRAMES 40
#define LOOP_MSG_RETRACE 666
#define LOOP_MSG_SP_DONE 667
#define LOOP_MSG_NOTIFY 670
#define LOOP_MSG_AUDIO_TASK 700
#define LOOP_MSG_AUDIO_DONE 701
#define LOOP_MSG_SI 800
#define LOOP_MSG_QUIT 999
#define LOOP_MSG_WATCHDOG 1666

static gc_sem_t sViSem; // never posted: the VI thread just sleeps on it for a frame
static volatile s32 sViRun;
static OSMesgQueue sIrqQ;
static OSMesg sIrqBuf[8];
static OSMesgQueue sSchedQ;
static OSMesg sSchedBuf[64];
static OSMesgQueue sSchedCmdQ;
static OSMesg sSchedCmdBuf[8];
static OSMesgQueue sAudioQ;
static OSMesg sAudioBuf[30];
static OSMesgQueue sAudioCmdQ;
static OSMesg sAudioCmdBuf[4];
static OSMesgQueue sPadQ;
static OSMesg sPadBuf[8];
static OSMesgQueue sSerialLockQ;
static OSMesg sSerialLockBuf[1];
static OSMesgQueue sSerialEventQ;
static OSMesg sSerialEventBuf[1];
static OSMesgQueue sMainQ;
static OSMesg sMainBuf[60];
static OSMesgQueue sGfxQ;
static OSMesg sGfxBuf[8];
static OSMesgQueue sClientDoneQ;
static OSMesg sClientDoneBuf[8];
static OSMesgQueue sLoopDoneQ;
static OSMesg sLoopDoneBuf[4];

static volatile s32 sGraphDone;
static volatile s32 sRetraces;
static volatile s32 sDropped;
static volatile s32 sFrames;
static volatile s32 sGraphTimeouts;
static volatile s32 sAudioTicks;
static volatile s32 sAudioTimeouts;
static volatile s32 sPadPolls;
static volatile s32 sMainRetraces;
static volatile s32 sSpDone;

static void LoopVi_Entry(void* arg) {
    for (;;) {
        gc_sem_wait_ticks(sViSem, GC_TB_HZ / 60);
        if (sViRun) {
            __gcPostEvent(OS_EVENT_VI);
        }
    }
}

static void LoopIrq_Entry(void* arg) {
    OSMesgQueue* clients[] = { &sSchedQ, &sPadQ, &sMainQ, &sAudioQ };
    OSMesg m = NULL;
    s32 i;

    while (!sGraphDone) {
        osRecvMesg(&sIrqQ, &m, OS_MESG_BLOCK);
        sRetraces++;
        for (i = 0; i < ARRAY_COUNT_(clients); i++) {
            if (osSendMesg(clients[i], MSG(LOOP_MSG_RETRACE), OS_MESG_NOBLOCK) != 0) {
                sDropped++;
            }
        }
    }
    sViRun = false;
    // Audio, pad and main first: audio still needs Sched to answer its last task
    for (i = 1; i < ARRAY_COUNT_(clients); i++) {
        osSendMesg(clients[i], MSG(LOOP_MSG_QUIT), OS_MESG_BLOCK);
    }
    for (i = 1; i < ARRAY_COUNT_(clients); i++) {
        osRecvMesg(&sClientDoneQ, NULL, OS_MESG_BLOCK);
    }
    osSendMesg(&sSchedQ, MSG(LOOP_MSG_QUIT), OS_MESG_BLOCK);
    osRecvMesg(&sClientDoneQ, NULL, OS_MESG_BLOCK);
    osSendMesg(&sLoopDoneQ, MSG(1), OS_MESG_BLOCK);
}

static void LoopSched_Entry(void* arg) {
    OSMesg m = NULL;
    s32 gfxDone = 0;

    for (;;) {
        osRecvMesg(&sSchedQ, &m, OS_MESG_BLOCK);
        switch (VAL(m)) {
            case LOOP_MSG_NOTIFY:
                // "Run" the queued task: the RSP stub completes it at once (sp.c posts OS_EVENT_SP)
                while (osRecvMesg(&sSchedCmdQ, NULL, OS_MESG_NOBLOCK) == 0) {
                    __gcPostEvent(OS_EVENT_SP);
                }
                break;

            case LOOP_MSG_SP_DONE:
                sSpDone++;
                gfxDone++;
                break;

            case LOOP_MSG_RETRACE:
                // Completion is reported on the next retrace, like a pending framebuffer swap
                for (; gfxDone > 0; gfxDone--) {
                    osSendMesg(&sGfxQ, MSG(1), OS_MESG_NOBLOCK);
                }
                break;

            case LOOP_MSG_AUDIO_TASK:
                osSendMesg(&sAudioCmdQ, MSG(LOOP_MSG_AUDIO_DONE), OS_MESG_NOBLOCK);
                break;

            case LOOP_MSG_QUIT:
                osSendMesg(&sClientDoneQ, MSG(16), OS_MESG_BLOCK);
                return;
        }
    }
}

static void LoopAudio_Entry(void* arg) {
    OSMesg m = NULL;
    OSTimer timer;

    for (;;) {
        osRecvMesg(&sAudioQ, &m, OS_MESG_BLOCK);
        if (VAL(m) == LOOP_MSG_QUIT) {
            break;
        }
        SpinUsec(2000); // AudioThread_Update
        osSendMesg(&sSchedQ, MSG(LOOP_MSG_AUDIO_TASK), OS_MESG_BLOCK);
        osSetTimer(&timer, OS_USEC_TO_CYCLES(32000), 0, &sAudioCmdQ, MSG(LOOP_MSG_WATCHDOG));
        osRecvMesg(&sAudioCmdQ, &m, OS_MESG_BLOCK);
        osStopTimer(&timer);
        if (VAL(m) == LOOP_MSG_WATCHDOG) {
            sAudioTimeouts++;
        }
        sAudioTicks++;
    }
    osSendMesg(&sClientDoneQ, MSG(11), OS_MESG_BLOCK);
}

static void LoopPad_Entry(void* arg) {
    OSMesg m = NULL;

    for (;;) {
        osRecvMesg(&sPadQ, &m, OS_MESG_BLOCK);
        if (VAL(m) == LOOP_MSG_QUIT) {
            break;
        }
        // PadMgr_AcquireSerialEventQueue / osContStartReadData / PadMgr_ReleaseSerialEventQueue
        osRecvMesg(&sSerialLockQ, &m, OS_MESG_BLOCK);
        __gcPostEvent(OS_EVENT_SI);
        osRecvMesg(&sSerialEventQ, NULL, OS_MESG_BLOCK);
        osSendMesg(&sSerialLockQ, m, OS_MESG_BLOCK);
        sPadPolls++;
    }
    osSendMesg(&sClientDoneQ, MSG(15), OS_MESG_BLOCK);
}

static void LoopMain_Entry(void* arg) {
    OSMesg m = NULL;

    for (;;) {
        osRecvMesg(&sMainQ, &m, OS_MESG_BLOCK);
        if (VAL(m) == LOOP_MSG_QUIT) {
            break;
        }
        sMainRetraces++;
    }
    osSendMesg(&sClientDoneQ, MSG(12), OS_MESG_BLOCK);
}

static void LoopGraph_Entry(void* arg) {
    OSMesg m = NULL;
    OSTimer timer;
    s32 i;

    osSendMesg(&sGfxQ, NULL, OS_MESG_BLOCK); // GameState_Init primes the queue (game.c)
    for (i = 0; i < LOOP_FRAMES; i++) {
        // Graph_TaskSet00: wait for the previous task with a 3 s watchdog
        osSetTimer(&timer, OS_USEC_TO_CYCLES(3 * 1000 * 1000), 0, &sGfxQ, MSG(LOOP_MSG_WATCHDOG));
        osRecvMesg(&sGfxQ, &m, OS_MESG_BLOCK);
        osStopTimer(&timer);
        if (VAL(m) == LOOP_MSG_WATCHDOG) {
            sGraphTimeouts++;
        }
        SpinUsec(5000); // GameState_Update
        osSendMesg(&sSchedCmdQ, MSG(i), OS_MESG_BLOCK);
        osSendMesg(&sSchedQ, MSG(LOOP_MSG_NOTIFY), OS_MESG_BLOCK);
        sFrames++;
    }
    // GameState_Destroy: blocking receive of the last task with no timeout
    osRecvMesg(&sGfxQ, &m, OS_MESG_BLOCK);
    sGraphDone = true;
    osSendMesg(&sLoopDoneQ, MSG(2), OS_MESG_BLOCK);
}

static void Test_FrameLoop(void) {
    gc_thread_t vi;
    OSTime start;
    u64 us;

    Test_Begin("mini frame loop (retrace fan-out, watchdog timers, SI lock token)");

    osCreateMesgQueue(&sIrqQ, sIrqBuf, ARRAY_COUNT_(sIrqBuf));
    osCreateMesgQueue(&sSchedQ, sSchedBuf, ARRAY_COUNT_(sSchedBuf));
    osCreateMesgQueue(&sSchedCmdQ, sSchedCmdBuf, ARRAY_COUNT_(sSchedCmdBuf));
    osCreateMesgQueue(&sAudioQ, sAudioBuf, ARRAY_COUNT_(sAudioBuf));
    osCreateMesgQueue(&sAudioCmdQ, sAudioCmdBuf, ARRAY_COUNT_(sAudioCmdBuf));
    osCreateMesgQueue(&sPadQ, sPadBuf, ARRAY_COUNT_(sPadBuf));
    osCreateMesgQueue(&sSerialLockQ, sSerialLockBuf, ARRAY_COUNT_(sSerialLockBuf));
    osCreateMesgQueue(&sSerialEventQ, sSerialEventBuf, ARRAY_COUNT_(sSerialEventBuf));
    osCreateMesgQueue(&sMainQ, sMainBuf, ARRAY_COUNT_(sMainBuf));
    osCreateMesgQueue(&sGfxQ, sGfxBuf, ARRAY_COUNT_(sGfxBuf));
    osCreateMesgQueue(&sClientDoneQ, sClientDoneBuf, ARRAY_COUNT_(sClientDoneBuf));
    osCreateMesgQueue(&sLoopDoneQ, sLoopDoneBuf, ARRAY_COUNT_(sLoopDoneBuf));
    osSendMesg(&sSerialLockQ, NULL, OS_MESG_BLOCK);
    osSetEventMesg(OS_EVENT_VI, &sIrqQ, MSG(LOOP_MSG_RETRACE));
    osSetEventMesg(OS_EVENT_SP, &sSchedQ, MSG(LOOP_MSG_SP_DONE));
    osSetEventMesg(OS_EVENT_SI, &sSerialEventQ, MSG(LOOP_MSG_SI));

    start = osGetTime();
    StartThread(0, 19, LoopIrq_Entry, NULL, 18);
    StartThread(1, 5, LoopSched_Entry, NULL, 16);
    StartThread(2, 7, LoopPad_Entry, NULL, 15);
    StartThread(3, 3, LoopMain_Entry, NULL, 12);
    StartThread(4, 10, LoopAudio_Entry, NULL, 11);
    StartThread(5, 4, LoopGraph_Entry, NULL, 9);
    CHECK_EQ(gc_sem_create(&sViSem, 0), 0);
    sViRun = true;
    CHECK_EQ(gc_thread_create(&vi, LoopVi_Entry, NULL, 0x2000, GC_PRIO_SERVICE_VI), 0);

    osRecvMesg(&sLoopDoneQ, NULL, OS_MESG_BLOCK);
    osRecvMesg(&sLoopDoneQ, NULL, OS_MESG_BLOCK);
    us = Usec(osGetTime() - start);

    gc_log("  %d frames, %d retraces in %u ms; audio %d, pad %d, main %d, SP done %d, dropped %d", (int)sFrames,
           (int)sRetraces, (unsigned int)(us / 1000), (int)sAudioTicks, (int)sPadPolls, (int)sMainRetraces,
           (int)sSpDone, (int)sDropped);
    CHECK_EQ(sFrames, LOOP_FRAMES);
    CHECK_EQ(sSpDone, LOOP_FRAMES);
    CHECK_EQ(sGraphTimeouts, 0);
    CHECK_EQ(sAudioTimeouts, 0);
    CHECK_EQ(sDropped, 0);
    CHECK(sRetraces >= LOOP_FRAMES);
    CHECK_EQ(sAudioTicks, sRetraces);
    CHECK_EQ(sPadPolls, sRetraces);
    CHECK_EQ(sMainRetraces, sRetraces);
    // 60 Hz retraces: each frame needs one, plus the priming frame
    CHECK(us >= (u64)(LOOP_FRAMES - 1) * 16000 && us < (u64)(LOOP_FRAMES + 10) * 17000);
}

/* -------------------------------------------------------------------------------------------- */
/* Boot: gc_ultra_boot() with stand-ins for the game's bootproc() and the VI shim                */
/* -------------------------------------------------------------------------------------------- */

static OSThread sRunnerThread;
static volatile s32 sRunnerRunning;
static s32 sBootprocCalls;
static s32 sBootOnNonGameThread;
static OSId sBootThreadId;
static s32 sRunnerRanDuringBootproc;
static s32 sViInitCalls;
static void Runner_Entry(void* arg);

void __gcViInit(void) {
    sViInitCalls++;
    gc_log("ultra_test: __gcViInit (stand-in, the VI shim is not linked)");
}

/* Same sequence as src/boot/boot_main.c, with the test runner in place of the Idle thread */
void bootproc(void) {
    sBootprocCalls++;
    sBootOnNonGameThread = (__gcGetCurrentThread() == NULL);
    sBootThreadId = osGetThreadId(NULL);
    osMemSize = osGetMemSize();
    osInitialize();
    osUnmapTLBAll();
    osCreateThread(&sRunnerThread, RUNNER_ID, Runner_Entry, NULL, NULL, RUNNER_PRI);
    osStartThread(&sRunnerThread);
    // The boot thread (libogc main, LWP 64) outranks the runner, which must not have run yet
    sRunnerRanDuringBootproc = sRunnerRunning;
}

static void Test_Boot(void) {
    u32 prev;

    Test_Begin("gc_ultra_boot, bootproc on the boot thread, globals");

    CHECK_EQ(sBootprocCalls, 1);
    CHECK_EQ(sViInitCalls, 1);
    CHECK(sBootOnNonGameThread);
    CHECK_EQ(sBootThreadId, 0);
    CHECK(!sRunnerRanDuringBootproc);
    CHECK_EQ(osMemSize, 0x800000);
    CHECK_EQ(osTvType, OS_TV_NTSC);
    CHECK_EQ(osResetType, 0);
    CHECK(((uintptr_t)osAppNMIBuffer & 7) == 0);
    CHECK(Usec(osGetTime()) < 10 * 1000 * 1000); // counts from gc_ultra_boot
    prev = __osSetFpcCsr(0x1234);
    CHECK_EQ(prev, FPCSR_FS | FPCSR_EV);
    CHECK_EQ(__osGetFpcCsr(), 0x1234);
    __osSetFpcCsr(prev);
    CHECK(__osGetCurrFaultedThread() == NULL);
}

/* -------------------------------------------------------------------------------------------- */

static void Runner_Entry(void* arg) {
    sRunnerRunning = true;
    gc_log("ULTRA_TEST start (runner thread id %d, LWP priority %d)", (int)osGetThreadId(NULL),
           __gcMapPriority(osGetThreadPri(NULL)));

    Test_Boot();
    Test_QueueBasics();
    Test_JamOrder();
    Test_ZeroQueue();
    Test_HigherReceiverPreempts();
    Test_LowerReceiverWaits();
    Test_TwoWaitersPriority();
    Test_FullQueueBlocks();
    Test_ThreadApi();
    Test_ThreadReuse();
    Test_DestroyStop();
    Test_TimeRate();
    Test_Timers();
    Test_IntMask();
    Test_Events();
    Test_Stress();
    Test_StopDestroySenders();
    Test_RecreateAndPriorities();
    Test_ConcurrentSleepers();
    Test_MaskedBlockingAndFarTimers();
    Test_FrameLoop();

    sCurrentTest = "(finished)";
    gc_log("ULTRA_TEST DONE: %d passed, %d failed", (int)sPassed, (int)sFailed);
    sDone = true;
}

int main(void) {
    u64 start;

    gc_ogc_video_init();
    gc_ogc_log_init();
    gc_ogc_sync_init();
    gc_log("ultra_test: libultra OS core test (threads, queues, events, intmask, time, timers)");

    // As port/gc/ogc/main.c: start the "game", then get out of the way
    gc_ultra_boot();

    // Lowest priority: the tests run whenever this thread would; it only wakes for the timeout
    gc_thread_set_prio(gc_thread_self(), GC_PRIO_IDLE);
    start = gc_time_ticks();
    while (!sDone) {
        gc_video_wait_vsync();
        if (gc_time_ticks() - start > TEST_TIMEOUT_SECONDS * GC_TB_HZ) {
            gc_log("ULTRA_TEST TIMEOUT in \"%s\" after %d s (%d passed, %d failed so far)", sCurrentTest,
                   TEST_TIMEOUT_SECONDS, (int)sPassed, (int)sFailed);
            break;
        }
    }
    for (;;) {
        gc_video_wait_vsync();
    }
    return 0;
}

/**
 * libultra timers: osSetTimer, osStopTimer, served by a shim timer thread.
 *
 * Active timers form a doubly linked list sorted by expiry, like libultra's __osTimerList, but
 * OSTimer.value holds the absolute expiry time in GameCube timebase ticks instead of a delta to the
 * previous timer (only the shim reads it). The timer thread runs at GC_PRIO_SERVICE_TIMER, above
 * every game thread, and sleeps on a semaphore until the earliest expiry. An expired timer is
 * unlinked first and then sends its message NOBLOCK (a full queue drops it, as on the N64); an
 * interval timer is re-armed.
 *
 * Membership in the list is checked by walking it rather than trusting t->next, so a timer that was
 * never set (stack garbage) or is set again while active cannot corrupt the list.
 */
#include "gc_os_core.h"

#define TIMER_THREAD_STACK_SIZE 0x4000

// libultra never programs the compare register less than 468 cycles (10 us) ahead. The same floor
// keeps a tiny interval timer from monopolising the timer thread.
#define TIMER_MIN_INTERVAL_CYCLES 468

static OSTimer sTimerList = { &sTimerList, &sTimerList, 0, 0, NULL, NULL };
static gc_sem_t sTimerSem;
static s32 sTimerInitialized;

/** `base + ticks`, saturating: a far-future deadline must not wrap around into the past. */
static u64 Timer_AddTicks(u64 base, u64 ticks) {
    return (ticks > (0xFFFFFFFFFFFFFFFFULL - base)) ? 0xFFFFFFFFFFFFFFFFULL : (base + ticks);
}

static s32 Timer_IsActiveLocked(OSTimer* t) {
    OSTimer* it;

    for (it = sTimerList.next; it != &sTimerList; it = it->next) {
        if (it == t) {
            return true;
        }
    }
    return false;
}

static void Timer_UnlinkLocked(OSTimer* t) {
    t->prev->next = t->next;
    t->next->prev = t->prev;
    t->next = NULL;
    t->prev = NULL;
}

/** Insert sorted by expiry; timers with the same expiry fire in the order they were set. */
static void Timer_InsertLocked(OSTimer* t) {
    OSTimer* it = sTimerList.next;

    while ((it != &sTimerList) && (it->value <= t->value)) {
        it = it->next;
    }
    t->next = it;
    t->prev = it->prev;
    it->prev->next = t;
    it->prev = t;
}

static void Timer_ThreadEntry(void* arg) {
    for (;;) {
        OSTimer* t;
        u64 now;
        u64 wait = 0;

        gc_os_lock();

        now = gc_time_ticks();
        while (((t = sTimerList.next) != &sTimerList) && (t->value <= now)) {
            Timer_UnlinkLocked(t);

            if (t->mq != NULL) {
                __gcSendMesgLocked(t->mq, t->msg, false);
            }

            if (t->interval != 0) {
                u64 interval = __gcCyclesToTicks((t->interval < TIMER_MIN_INTERVAL_CYCLES) ? TIMER_MIN_INTERVAL_CYCLES
                                                                                           : t->interval);

                t->value = Timer_AddTicks(t->value, interval);
                // Fell behind by more than one period: do not fire a burst to catch up
                if (t->value <= now) {
                    t->value = Timer_AddTicks(now, interval);
                }
                Timer_InsertLocked(t);
            }
        }

        if (t != &sTimerList) {
            wait = t->value - now;
        }

        gc_os_unlock();

        if (wait == 0) {
            gc_sem_wait(sTimerSem);
        } else {
            gc_sem_wait_ticks(sTimerSem, wait);
        }
    }
}

void __gcTimerInit(void) {
    gc_thread_t thread;

    if (sTimerInitialized) {
        return;
    }
    if (gc_sem_create(&sTimerSem, 0) != 0) {
        gc_halt("timer: cannot create the timer semaphore");
    }
    sTimerInitialized = true;
    if (gc_thread_create(&thread, Timer_ThreadEntry, NULL, TIMER_THREAD_STACK_SIZE, GC_PRIO_SERVICE_TIMER) != 0) {
        gc_halt("timer: cannot create the timer thread");
    }
}

int osSetTimer(OSTimer* t, OSTime countdown, OSTime interval, OSMesgQueue* mq, OSMesg msg) {
    s32 isFirst;

    if (!sTimerInitialized) {
        gc_halt("osSetTimer called before __gcTimerInit");
    }

    gc_os_lock();

    if (Timer_IsActiveLocked(t)) {
        Timer_UnlinkLocked(t);
    }

    t->interval = interval;
    t->mq = mq;
    t->msg = msg;
    // A countdown of 0 means "first expiry after one interval"
    t->value = Timer_AddTicks(gc_time_ticks(), __gcCyclesToTicks((countdown != 0) ? countdown : interval));
    Timer_InsertLocked(t);
    isFirst = (sTimerList.next == t);

    gc_os_unlock();

    // The timer thread sleeps until the earliest expiry, so it only needs waking when that changes.
    // Removing timers never needs a wakeup: the thread just wakes early and finds nothing due.
    if (isFirst) {
        gc_sem_post(sTimerSem);
    }

    return 0;
}

int osStopTimer(OSTimer* t) {
    s32 ret = -1;

    gc_os_lock();

    if (Timer_IsActiveLocked(t)) {
        Timer_UnlinkLocked(t);
        ret = 0;
    }

    gc_os_unlock();

    return ret;
}

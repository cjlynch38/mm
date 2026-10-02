/**
 * Synchronisation for the libultra shim: the global OS lock and condition variable, counting
 * semaphores with timeouts, and interrupt masking.
 *
 * libogc 3.x has LWP semaphores but no timed semaphore wait, so gc_sem_* is built from one shared
 * mutex, a condition variable per semaphore and a counter. LWP_CondTimedWait takes a relative
 * timeout and returns ETIMEDOUT (116) when it expires; the waits below re-check the counter and
 * the deadline in a loop, so early or spurious wakeups are harmless. A timeout that converts to 0
 * ticks returns ETIMEDOUT at once, without waiting (and without releasing the mutex).
 * libogc has fixed pools of 64 mutexes, 64 condition variables and 64 semaphores for the LWP_* API
 * (its own drivers, libfat and libiso9660 use kernel objects and take none of them).
 *
 * The kernel spins forever with interrupts disabled when a thread unlocks a mutex it does not own
 * or waits on a condition variable without owning the mutex, which freezes the console without a
 * word. The OS lock checks its owner first and halts with a message instead.
 */
#include <gccore.h>
#include <ogc/cond.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <time.h>
#include "gc_ogc.h"

/* The libultra shim needs one per game thread (up to 32), plus the timer and flash semaphores */
#define MAX_SEMS 48

/* Longest single condition wait; longer timeouts loop. Keeps the timespec small. */
#define MAX_WAIT_TICKS (3600ull * GC_TB_HZ)

static mutex_t sOsMutex = LWP_MUTEX_NULL;
static cond_t sOsCond = LWP_COND_NULL;
static volatile lwp_t sOsOwner = LWP_THREAD_NULL;

typedef struct {
    cond_t cond;
    unsigned int count;
} GcSem;

static mutex_t sSemMutex = LWP_MUTEX_NULL;
static GcSem sSems[MAX_SEMS];
static unsigned int sNumSems;

void gc_ogc_sync_init(void) {
    if (sOsMutex != LWP_MUTEX_NULL) {
        return;
    }
    if (LWP_MutexInit(&sOsMutex, false) != 0 || LWP_CondInit(&sOsCond) != 0 ||
        LWP_MutexInit(&sSemMutex, false) != 0) {
        gc_halt("gc_ogc_sync_init: cannot create the OS lock");
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Global OS lock                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

void gc_os_lock(void) {
    lwp_t self = LWP_GetSelf();

    // Only the owner itself can have stored its own handle, so this check needs no lock.
    if (sOsOwner == self) {
        gc_halt("gc_os_lock: thread %08X already holds the OS lock", self);
    }
    LWP_MutexLock(sOsMutex);
    sOsOwner = self;
}

void gc_os_unlock(void) {
    lwp_t self = LWP_GetSelf();

    if (sOsOwner != self) {
        gc_halt("gc_os_unlock: thread %08X does not hold the OS lock", self);
    }
    sOsOwner = LWP_THREAD_NULL;
    LWP_MutexUnlock(sOsMutex);
}

void gc_os_wait(void) {
    lwp_t self = LWP_GetSelf();

    if (sOsOwner != self) {
        gc_halt("gc_os_wait: thread %08X does not hold the OS lock", self);
    }
    sOsOwner = LWP_THREAD_NULL;
    LWP_CondWait(sOsCond, sOsMutex);
    sOsOwner = self;
}

void gc_os_broadcast(void) {
    LWP_CondBroadcast(sOsCond);
}

/* ---------------------------------------------------------------------------------------------- */
/* Semaphores                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

static GcSem* sem_get(gc_sem_t sem) {
    if (sem == 0 || sem > sNumSems) {
        gc_halt("gc_sem: invalid semaphore %u", sem);
    }
    return &sSems[sem - 1];
}

int gc_sem_create(gc_sem_t* out, unsigned int initial_count) {
    GcSem* s;

    LWP_MutexLock(sSemMutex);
    if (sNumSems >= MAX_SEMS) {
        LWP_MutexUnlock(sSemMutex);
        gc_log("gc_sem_create: more than %d semaphores", MAX_SEMS);
        return -1;
    }
    s = &sSems[sNumSems];
    if (LWP_CondInit(&s->cond) != 0) {
        LWP_MutexUnlock(sSemMutex);
        gc_log("gc_sem_create: out of LWP condition variables");
        return -1;
    }
    s->count = initial_count;
    sNumSems++;
    *out = sNumSems;
    LWP_MutexUnlock(sSemMutex);
    return 0;
}

void gc_sem_post(gc_sem_t sem) {
    GcSem* s = sem_get(sem);

    LWP_MutexLock(sSemMutex);
    s->count++;
    LWP_CondSignal(s->cond);
    LWP_MutexUnlock(sSemMutex);
}

void gc_sem_wait(gc_sem_t sem) {
    GcSem* s = sem_get(sem);

    LWP_MutexLock(sSemMutex);
    while (s->count == 0) {
        LWP_CondWait(s->cond, sSemMutex);
    }
    s->count--;
    LWP_MutexUnlock(sSemMutex);
}

int gc_sem_wait_ticks(gc_sem_t sem, unsigned long long ticks) {
    GcSem* s = sem_get(sem);
    u64 deadline;
    int result = 0;

    if (ticks > (1ull << 62)) {
        ticks = 1ull << 62;
    }
    deadline = gettime() + ticks;

    LWP_MutexLock(sSemMutex);
    while (s->count == 0) {
        u64 now = gettime();
        u64 left;
        struct timespec timeout;

        if (now >= deadline) {
            result = 1;
            break;
        }
        left = deadline - now;
        if (left > MAX_WAIT_TICKS) {
            left = MAX_WAIT_TICKS;
        }
        timeout.tv_sec = left / GC_TB_HZ;
        // LWP converts back with ns * 81 / 2000, i.e. the timebase runs at 40.5 ticks per us
        timeout.tv_nsec = (long)((left % GC_TB_HZ) * 2000 / 81);
        LWP_CondTimedWait(s->cond, sSemMutex, &timeout);
    }
    if (result == 0) {
        s->count--;
    }
    LWP_MutexUnlock(sSemMutex);
    return result;
}

/* ---------------------------------------------------------------------------------------------- */
/* Interrupt masking                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/* libogc's _CPU_ISR_Disable clears MSR[EE] and MSR[FP]. Clearing FP only makes the next FPU
 * instruction take the kernel's lazy FPU-switch path, so floating point keeps working.
 * The level is the old MSR[EE] bit (0x8000 or 0), and _CPU_ISR_Restore ORs it into the MSR, so
 * gc_irq_restore keeps only that bit: a stale or corrupted level must not set other MSR bits. */
unsigned int gc_irq_disable(void) {
    unsigned int level;

    _CPU_ISR_Disable(level);
    return level;
}

void gc_irq_restore(unsigned int level) {
    _CPU_ISR_Restore(level & MSR_EE);
}

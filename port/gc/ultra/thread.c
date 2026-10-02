/**
 * libultra threads (osCreateThread, osStartThread, osDestroyThread, priorities, the active-thread
 * list) on bridge (LWP) threads, and the wait helper the message queues block in.
 *
 * Each started OSThread runs on a "worker": an LWP thread with a shim-owned stack (the N64 stack
 * passed to osCreateThread is ignored, PPC frames are much larger). Workers live in a fixed table
 * and go back to a pool when a game thread's entry function returns, because the bridge never
 * frees thread stacks and the game starts short-lived threads again and again (sys_flashrom,
 * sys_slowly).
 *
 * The OSThread keeps the libultra fields the game and fault.c read: id, priority, state, flags,
 * the active-thread list (tlnext, ending at a sentinel whose priority is -1) and context.pc/sp/ra.
 *
 * Stopping or destroying ANOTHER thread never suspends its LWP thread: it could be queued on the
 * OS lock, and a mutex handed to a suspended thread would hang every game thread. The request is
 * recorded instead and the target acts on it at its next libultra call that can block or touches a
 * message queue (__gcOsCheckpointLocked, __gcOsWaitLocked). In the game this happens to threads
 * that already finished (sys_flashrom, sys_slowly), to the CPU-only sys_slowly filter thread (it
 * finishes its callback first) and to the Graph thread after a reset (main.c).
 */
#include "gc_os_core.h"

#define GC_MAX_WORKERS 32
#define GC_WORKER_STACK_SIZE 0x10000

// OSThread.state of a thread that finished or was destroyed: osStartThread() ignores it
#define GC_THREAD_STATE_DEAD 0

typedef struct GcWorker {
    OSThread* thread;      // game thread running on this worker, NULL while the worker is pooled
    void (*entry)(void*);  // job handed over by osStartThread()
    void* arg;
    gc_thread_t handle;    // LWP thread, valid when hasHandle
    gc_sem_t startSem;     // posted once per job
    GcIntMaskState intMask;
    u8 created;   // startSem and the LWP thread exist (or are being created)
    u8 hasHandle;
    u8 dead;      // the LWP thread exited (osDestroyThread on itself): never reused
    u8 stopped;   // osStopThread(): wait inside the shim until osStartThread()
    u8 destroyed; // destroyed or re-created by another thread: exit at the next checkpoint
    OSThread orphan; // stands in for `thread` once the game destroyed or re-created that OSThread
} GcWorker;

static GcWorker sWorkers[GC_MAX_WORKERS];

// Number of workers with `stopped` or `destroyed` set: lets the checkpoint skip the lookup
static s32 sNumPendingRequests;

static __OSThreadTail sThreadTail = { NULL, -1 };
static OSThread* sActiveQueue = (OSThread*)&sThreadTail;

// "The current thread" for the boot thread and shim service threads, which have no OSThread
static OSThread sNonGameThread;
static GcIntMaskState sNonGameIntMask = { OS_IM_ALL, 0 };

int __gcMapPriority(OSPri pri) {
    if (pri <= OS_PRIORITY_IDLE) {
        return GC_PRIO_IDLE;
    }
    if (pri < GC_PRIO_GAME_MIN) {
        return GC_PRIO_GAME_MIN;
    }
    if (pri > GC_PRIO_GAME_MAX) {
        return GC_PRIO_GAME_MAX;
    }
    return pri;
}

/** The worker the calling thread runs on, or NULL. Lock-free: a thread only ever matches the entry
 *  whose handle it wrote itself before running game code. */
static GcWorker* Worker_Self(void) {
    gc_thread_t self = gc_thread_self();
    GcWorker* w;

    for (w = sWorkers; w < &sWorkers[GC_MAX_WORKERS]; w++) {
        if (w->hasHandle && !w->dead && (w->handle == self)) {
            return w;
        }
    }
    return NULL;
}

static GcWorker* Worker_FindByThread(OSThread* t) {
    GcWorker* w;

    if (t == NULL) {
        return NULL;
    }
    for (w = sWorkers; w < &sWorkers[GC_MAX_WORKERS]; w++) {
        if (w->thread == t) {
            return w;
        }
    }
    return NULL;
}

/** A pooled worker if there is one, else an unused slot. Lock held. */
static GcWorker* Worker_Alloc(void) {
    GcWorker* unused = NULL;
    GcWorker* w;

    for (w = sWorkers; w < &sWorkers[GC_MAX_WORKERS]; w++) {
        if ((w->thread != NULL) || w->dead) {
            continue;
        }
        if (w->created) {
            return w;
        }
        if (unused == NULL) {
            unused = w;
        }
    }
    return unused;
}

static void Worker_SetStoppedLocked(GcWorker* w, s32 stopped) {
    if (!w->stopped && stopped) {
        sNumPendingRequests++;
    } else if (w->stopped && !stopped) {
        sNumPendingRequests--;
    }
    w->stopped = stopped;
}

static void Worker_SetDestroyedLocked(GcWorker* w) {
    if (!w->destroyed) {
        sNumPendingRequests++;
    }
    w->destroyed = true;
}

static void ActiveList_RemoveLocked(OSThread* t) {
    OSThread** link = &sActiveQueue;

    while (*link != (OSThread*)&sThreadTail) {
        if (*link == t) {
            // t->tlnext is left alone: fault.c may be walking the list without the lock
            *link = t->tlnext;
            return;
        }
        link = &(*link)->tlnext;
    }
}

/** The game thread on `w` is finished: take it off the active list and return the worker to the
 *  pool. Lock held. */
static void Worker_ReleaseLocked(GcWorker* w) {
    OSThread* t = w->thread;

    if ((t != NULL) && (t != &w->orphan)) {
        ActiveList_RemoveLocked(t);
        t->state = GC_THREAD_STATE_DEAD;
    }
    Worker_SetStoppedLocked(w, false);
    if (w->destroyed) {
        sNumPendingRequests--;
        w->destroyed = false;
    }
    w->thread = NULL;
    w->entry = NULL;
    w->arg = NULL;
}

/** Terminate the calling thread (libultra osDestroyThread on itself). Lock held, never returns. */
static void Worker_ExitLocked(GcWorker* w) __attribute__((noreturn));
static void Worker_ExitLocked(GcWorker* w) {
    Worker_ReleaseLocked(w);
    w->dead = true;
    gc_os_unlock();
    gc_thread_exit();
}

/**
 * The game destroyed or re-created the OSThread running on `w` from another thread. Give the worker
 * a private copy of the OSThread so it never touches the game's one again, and make it exit at its
 * next checkpoint. Lock held.
 */
static void Worker_DetachLocked(GcWorker* w) {
    if (w->thread != &w->orphan) {
        w->orphan = *w->thread;
        w->orphan.tlnext = (OSThread*)&sThreadTail;
        w->thread = &w->orphan;
    }
    Worker_SetDestroyedLocked(w);
    // Wake it if it is blocked in the shim
    gc_os_broadcast();
}

/** Act on osStopThread()/osDestroyThread() requests for the calling worker. Lock held. */
static void Worker_HonorRequestsLocked(GcWorker* w) {
    while (w->stopped || w->destroyed) {
        if (w->destroyed) {
            Worker_ExitLocked(w);
        }
        gc_os_wait();
    }
}

void __gcOsCheckpointLocked(void) {
    GcWorker* w;

    if (sNumPendingRequests == 0) {
        return;
    }
    w = Worker_Self();
    if (w != NULL) {
        Worker_HonorRequestsLocked(w);
    }
}

void __gcOsWaitLocked(void) {
    GcWorker* w = Worker_Self();

    if (w == NULL) {
        gc_os_wait();
        return;
    }

    Worker_HonorRequestsLocked(w);
    w->thread->state = OS_STATE_WAITING;
    gc_os_wait();
    Worker_HonorRequestsLocked(w);
    w->thread->state = OS_STATE_RUNNING;
}

static void Worker_Main(void* arg) {
    GcWorker* w = (GcWorker*)arg;
    void (*entry)(void*);
    void* entryArg;

    // The creator also stores the handle gc_thread_create() returned, but only after this thread
    // may already have run (it can preempt its creator). Game code identifies threads by
    // gc_thread_self(), so that is the value that counts.
    gc_os_lock();
    w->handle = gc_thread_self();
    w->hasHandle = true;
    gc_os_unlock();

    for (;;) {
        gc_sem_wait(w->startSem);

        gc_os_lock();
        // Stopped or destroyed between osStartThread() and now
        while (w->stopped && !w->destroyed) {
            gc_os_wait();
        }
        if ((w->thread == NULL) || w->destroyed || (w->entry == NULL)) {
            Worker_ReleaseLocked(w);
            gc_os_unlock();
            continue;
        }
        entry = w->entry;
        entryArg = w->arg;
        w->thread->state = OS_STATE_RUNNING;
        gc_os_unlock();

        entry(entryArg);

        // The entry function returned. libultra returns into __osCleanupThread, which destroys the
        // thread; the worker goes back to the pool instead.
        gc_os_lock();
        Worker_ReleaseLocked(w);
        gc_os_unlock();
    }
}

/** Create the LWP thread for a worker slot whose first job is already assigned. */
static void Worker_Create(GcWorker* w, int prio, OSId id) {
    gc_thread_t handle;
    int curPrio;

    // Count 1: the first job is already assigned
    if (gc_sem_create(&w->startSem, 1) != 0) {
        gc_halt("osStartThread: cannot create a semaphore for thread %d", (int)id);
    }
    if (gc_thread_create(&handle, Worker_Main, w, GC_WORKER_STACK_SIZE, prio) != 0) {
        gc_halt("osStartThread: cannot create an LWP thread for thread %d", (int)id);
    }

    gc_os_lock();
    if (!w->hasHandle) {
        w->handle = handle;
        w->hasHandle = true;
    }
    // osSetThreadPri() cannot reach the worker before its handle is known: catch up here, under the
    // lock so that a later osSetThreadPri() is never overwritten by this value
    curPrio = (w->thread != NULL) ? __gcMapPriority(w->thread->priority) : prio;
    if (curPrio != prio) {
        gc_thread_set_prio(w->handle, curPrio);
    }
    gc_os_unlock();
}

void osCreateThread(OSThread* thread, OSId id, void* entry, void* arg, void* sp, OSPri p) {
    GcWorker* w;

    gc_os_lock();

    w = Worker_FindByThread(thread);
    if (w != NULL) {
        // Re-created while still running (libultra would corrupt its queues): the old worker
        // behaves as if destroyed
        Worker_DetachLocked(w);
    }
    // Never link an OSThread twice
    ActiveList_RemoveLocked(thread);

    thread->id = id;
    thread->priority = p;
    thread->next = NULL;
    thread->queue = NULL;
    // Only for display by fault.c; the worker reads entry and arg from here at osStartThread()
    thread->context.pc = (u32)entry;
    thread->context.a0 = (intptr_t)arg;
    thread->context.sp = (u64)(s32)sp - 16;
    thread->context.ra = (intptr_t)__osCleanupThread;
    thread->context.fpcsr = (u32)(FPCSR_FS | FPCSR_EV);
    thread->fp = 0;
    thread->state = OS_STATE_STOPPED;
    thread->flags = 0;

    thread->tlnext = sActiveQueue;
    sActiveQueue = thread;

    gc_os_unlock();

    gc_log("osCreateThread: id %d pri %d -> lwp %d entry %p", (int)id, (int)p, __gcMapPriority(p), entry);
}

void osStartThread(OSThread* t) {
    GcWorker* w;
    s32 isNew;
    int prio;

    gc_os_lock();

    w = Worker_FindByThread(t);
    if (w != NULL) {
        // Already running: only a thread stopped by osStopThread() needs a restart
        if (w->stopped) {
            Worker_SetStoppedLocked(w, false);
            t->state = OS_STATE_RUNNABLE;
            gc_os_broadcast();
        }
        gc_os_unlock();
        return;
    }

    if (t->state != OS_STATE_STOPPED) {
        // Finished or destroyed
        gc_os_unlock();
        gc_log("osStartThread: thread %d is finished, not started again", (int)t->id);
        return;
    }

    w = Worker_Alloc();
    if (w == NULL) {
        gc_os_unlock();
        gc_halt("osStartThread: more than %d threads (thread %d)", GC_MAX_WORKERS, (int)t->id);
    }

    w->thread = t;
    w->entry = (void (*)(void*))(uintptr_t)t->context.pc;
    w->arg = (void*)(intptr_t)t->context.a0;
    w->intMask.mask = OS_IM_ALL;
    w->intMask.irqLevel = 0;
    t->state = OS_STATE_RUNNABLE;
    prio = __gcMapPriority(t->priority);
    isNew = !w->created;
    w->created = true;
    if (!isNew) {
        // A pooled worker still has its previous job's priority. Set the new one under the lock, so
        // an osSetThreadPri() on `t` from another thread cannot be overwritten by this older value.
        gc_thread_set_prio(w->handle, prio);
    }

    gc_os_unlock();

    // A higher-priority thread preempts the caller right here, as on the N64
    if (isNew) {
        Worker_Create(w, prio, t->id);
    } else {
        gc_sem_post(w->startSem);
    }
}

void osStopThread(OSThread* t) {
    GcWorker* self;
    GcWorker* w;

    gc_os_lock();

    self = Worker_Self();
    w = (t == NULL) ? self : Worker_FindByThread(t);
    if (w == NULL) {
        // Not started yet (it stays OS_STATE_STOPPED), finished, or not a game thread
        gc_os_unlock();
        return;
    }

    Worker_SetStoppedLocked(w, true);
    w->thread->state = OS_STATE_STOPPED;
    if (w == self) {
        Worker_HonorRequestsLocked(self);
        self->thread->state = OS_STATE_RUNNING;
    }

    gc_os_unlock();
}

void osDestroyThread(OSThread* t) {
    GcWorker* self;
    GcWorker* w;

    gc_os_lock();

    self = Worker_Self();
    w = (t == NULL) ? self : Worker_FindByThread(t);

    if ((w != NULL) && (w == self)) {
        Worker_ExitLocked(self);
    }

    if (t == NULL) {
        gc_os_unlock();
        gc_log("osDestroyThread(NULL) on a non-game thread ignored");
        return;
    }

    ActiveList_RemoveLocked(t);
    if (w != NULL) {
        Worker_DetachLocked(w);
    }
    t->state = GC_THREAD_STATE_DEAD;

    gc_os_unlock();
}

void osYieldThread(void) {
    gc_thread_yield();
}

void osSetThreadPri(OSThread* t, OSPri p) {
    GcWorker* self;
    GcWorker* w;
    s32 apply = false;
    gc_thread_t handle = 0;

    gc_os_lock();

    self = Worker_Self();
    if (t == NULL) {
        w = self;
        t = (w != NULL) ? w->thread : &sNonGameThread;
    } else {
        w = Worker_FindByThread(t);
    }

    t->priority = p;
    if ((w != NULL) && w->hasHandle) {
        if (w == self) {
            apply = true;
            handle = w->handle;
        } else {
            // Another thread: under the lock, so concurrent changes are applied in order
            gc_thread_set_prio(w->handle, __gcMapPriority(p));
        }
    }

    gc_os_unlock();

    // The caller itself: outside the lock, so a thread lowering itself does not hold the lock while
    // it is preempted
    if (apply) {
        gc_thread_set_prio(handle, __gcMapPriority(p));
    }
}

static OSThread* Thread_CurrentOrDummy(void) {
    OSThread* t = __gcGetCurrentThread();

    return (t != NULL) ? t : &sNonGameThread;
}

OSPri osGetThreadPri(OSThread* t) {
    if (t == NULL) {
        t = Thread_CurrentOrDummy();
    }
    return t->priority;
}

OSId osGetThreadId(OSThread* t) {
    if (t == NULL) {
        t = Thread_CurrentOrDummy();
    }
    return t->id;
}

OSThread* __gcGetCurrentThread(void) {
    GcWorker* w = Worker_Self();

    return (w != NULL) ? w->thread : NULL;
}

GcIntMaskState* __gcGetIntMaskState(void) {
    GcWorker* w = Worker_Self();

    return (w != NULL) ? &w->intMask : &sNonGameIntMask;
}

/** libultra makes this the return address of every thread entry. Workers do not return into it
 *  (they go back to the pool), but fault.c compares stack frames against its address. */
void __osCleanupThread(void) {
    osDestroyThread(NULL);
}

OSThread* __osGetActiveQueue(void) {
    return sActiveQueue;
}

OSThread* __osGetCurrFaultedThread(void) {
    // CPU exceptions are handled by libogc, never reported as faulted libultra threads
    return NULL;
}

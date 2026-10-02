/**
 * Threads for the libultra shim, on libogc LWP.
 *
 * In libogc 3.x, LWP is a thin layer over the "tuxedo" kernel. Its behaviour, read from the
 * disassembly of libogc.a (lwpc_thread.o, thread.o, sync.o) since only headers ship:
 *  - LWP priority p becomes kernel priority 127 - p: LWP 127 is the highest, 0 the lowest.
 *  - Scheduling is strictly by priority, without time slicing. A thread runs until it blocks,
 *    yields or a higher-priority thread becomes ready (also from an interrupt). Disabling
 *    interrupts therefore also disables preemption, as on N64.
 *  - Threads of equal priority are kept in creation order. LWP_YieldThread switches to the next
 *    ready thread of the same priority; whenever a thread blocks, the earliest-created ready
 *    thread of the highest priority runs. There is no round robin.
 *  - Wait queues (mutexes, condition variables, semaphores) are ordered by priority and FIFO
 *    within a priority. Mutexes use priority inheritance.
 *  - LWP_CreateThread mallocs a 1256-byte control block, writes the handle and only then starts
 *    the thread, which may run before LWP_CreateThread returns if it has a higher priority.
 *  - LWP_SuspendThread nests (each suspend needs a resume); LWP_ResumeThread on a thread that is
 *    not suspended does nothing. gc_thread_suspend never nests, to match osStopThread.
 *  - A suspended thread that waits on a mutex still receives it: unlocking hands the mutex to the
 *    first waiter whether it is suspended or not, and every other waiter then blocks until it is
 *    resumed. Only suspend a thread that cannot be waiting on any lock (gc_halt is the exception:
 *    nothing runs after it anyway).
 *  - LWP_SetThreadPriority ignores LWP_THREAD_NULL; it does not mean "the calling thread".
 *  - LWP has no exit call; gc_thread_exit uses the kernel's KThreadExit. The control block of an
 *    exited thread is only freed by LWP_JoinThread, which nothing calls (1256 bytes leak per exit).
 */
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <tuxedo/thread.h>
#include "gc_ogc.h"

#define MAX_THREADS 64
#define DEFAULT_STACK_SIZE 0x4000
#define MAX_STACK_SIZE 0x100000

typedef struct {
    void (*entry)(void* arg);
    void* arg;
    unsigned int slot;
} ThreadStart;

/* Every thread created through the bridge, so gc_halt can stop them all */
static lwp_t sThreads[MAX_THREADS];
static unsigned int sNumThreads;

static void* thread_trampoline(void* param) {
    ThreadStart* start = param;
    void (*entry)(void* arg) = start->entry;
    void* arg = start->arg;

    // Registered before the halted check: a gc_halt() from now on either sees this thread in
    // sThreads and suspends it, or has already set the flag tested below.
    sThreads[start->slot] = LWP_GetSelf();
    if (gc_ogc_halted()) {
        // Created after gc_halt() stopped the bridge threads (by a thread it does not stop, such as
        // libogc's main thread during boot): never start running game code.
        for (;;) {
            LWP_SuspendThread(LWP_GetSelf());
        }
    }
    entry(arg);
    return NULL;
}

static int clamp_prio(int prio) {
    if (prio < LWP_PRIO_IDLE) {
        return LWP_PRIO_IDLE;
    }
    if (prio > LWP_PRIO_HIGHEST) {
        return LWP_PRIO_HIGHEST;
    }
    return prio;
}

static lwp_t resolve(gc_thread_t thread) {
    return (thread == 0 || thread == LWP_THREAD_NULL) ? LWP_GetSelf() : thread;
}

int gc_thread_create(gc_thread_t* out, void (*entry)(void* arg), void* arg, unsigned int stack_size,
                     int prio) {
    unsigned int headerSize = (sizeof(ThreadStart) + 31) & ~31u;
    unsigned int level;
    unsigned int slot;
    ThreadStart* start;
    lwp_t handle = LWP_THREAD_NULL;
    lwp_t* handleOut = (out != NULL) ? (lwp_t*)out : &handle;
    unsigned char* mem;

    if (stack_size == 0) {
        stack_size = DEFAULT_STACK_SIZE;
    }
    if (stack_size > MAX_STACK_SIZE) {
        // Also keeps the rounding below from wrapping to 0, which LWP would replace by its 8 KB
        // default while still using our (then far too small) block as the stack.
        gc_log("gc_thread_create: stack size %u is too large", stack_size);
        return -1;
    }
    stack_size = (stack_size + 31) & ~31u;

    _CPU_ISR_Disable(level);
    slot = sNumThreads;
    if (slot < MAX_THREADS) {
        sNumThreads++;
    }
    _CPU_ISR_Restore(level);
    if (slot >= MAX_THREADS) {
        gc_log("gc_thread_create: more than %d threads", MAX_THREADS);
        return -1;
    }

    // The start record sits below the stack, where only a full stack overflow would reach it,
    // and the thread copies it into locals before running any game code.
    mem = gc_mem_alloc(headerSize + stack_size, 32);
    if (mem == NULL) {
        sThreads[slot] = LWP_THREAD_NULL;
        return -1;
    }
    start = (ThreadStart*)mem;
    start->entry = entry;
    start->arg = arg;
    start->slot = slot;

    // LWP_CreateThread stores the handle in *handleOut before the thread can run, so a
    // higher-priority thread already finds it in the caller's variable.
    if (LWP_CreateThread(handleOut, thread_trampoline, start, mem + headerSize, stack_size, clamp_prio(prio)) != 0) {
        sThreads[slot] = LWP_THREAD_NULL;
        gc_log("gc_thread_create: LWP_CreateThread failed (prio %d, stack %u)", prio, stack_size);
        return -1;
    }
    sThreads[slot] = *handleOut;
    return 0;
}

gc_thread_t gc_thread_self(void) {
    return LWP_GetSelf();
}

void gc_thread_set_prio(gc_thread_t thread, int prio) {
    LWP_SetThreadPriority(resolve(thread), clamp_prio(prio));
}

void gc_thread_yield(void) {
    LWP_YieldThread();
}

void gc_thread_suspend(gc_thread_t thread) {
    lwp_t t = resolve(thread);
    unsigned int level;

    if (t == LWP_GetSelf()) {
        // The caller is running, so it cannot already be suspended.
        LWP_SuspendThread(t);
        return;
    }
    _CPU_ISR_Disable(level);
    if (!LWP_ThreadIsSuspended(t)) {
        LWP_SuspendThread(t);
    }
    _CPU_ISR_Restore(level);
}

void gc_thread_resume(gc_thread_t thread) {
    LWP_ResumeThread(resolve(thread));
}

void gc_thread_exit(void) {
    KThreadExit(0);
    for (;;) {}
}

void gc_ogc_stop_threads(void) {
    lwp_t self = LWP_GetSelf();
    unsigned int level;
    unsigned int i;

    // With interrupts off nothing is rescheduled until every thread is suspended.
    _CPU_ISR_Disable(level);
    for (i = 0; i < sNumThreads; i++) {
        lwp_t t = sThreads[i];

        if (t != LWP_THREAD_NULL && t != 0 && t != self && !LWP_ThreadIsSuspended(t)) {
            LWP_SuspendThread(t);
        }
    }
    _CPU_ISR_Restore(level);
}

unsigned int gc_ogc_thread_count(void) {
    return sNumThreads;
}

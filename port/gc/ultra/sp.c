/**
 * libultra RSP/RDP API: osSpTask*, SP/DP status, osAfterPreNMI.
 *
 * A task runs synchronously inside osSpTaskStartGo(), on the CPU: graphics tasks go through the GX
 * renderer (gc_gfx_run_task, port/gc/gfx) when it is enabled, audio command lists through
 * Gc_AudRunTask (port/gc/audio) and NJPEG decodes through Gc_NJpegRunTask (port/gc/game/njpeg_cpu.c).
 * Other task types are skipped. It then posts the completion events the hardware would raise.
 * sched.c registers OS_EVENT_SP (RSP_DONE_MSG) and OS_EVENT_DP (RDP_DONE_MSG) on its own interrupt
 * queue and completes a task only when every unit it was dispatched to has reported done; a missing
 * event hangs Graph_TaskSet00/GameState_Destroy, an extra one completes the wrong task. What the game
 * sends:
 *   - M_GFXTASK (graph.c) has OS_SC_NEEDS_RSP | OS_SC_NEEDS_RDP and ends in gDPFullSync, so the
 *     scheduler waits for one SP and one DP event.
 *   - M_NJPEGTASK (z_jpeg.c) and the JP-only CIC task are RSP-only: one SP event.
 *   - M_AUDTASK would be RSP-only too, but the GameCube audio manager runs its command lists itself
 *     (Gc_AudioMgrRunTask, TARGET_GC code in audio_thread_manager.c) instead of queueing them behind
 *     the graphics tasks the Sched thread renders. The case below covers any other sender.
 * The events are queued on the Sched thread's own queue, so they are handled after Sched_RunTask
 * has recorded curRSPTask/curRDPTask.
 *
 * The status registers go through IO_READ/IO_WRITE, which TARGET_GC routes to the emulated
 * register block in io.c (the RSP always reads as halted, the RDP as idle).
 */
#include "gc_ultra_internal.h"

static OSTask* sSpLoadedTask;
static u32 sSpTaskCount[M_NJPEGTASK + 2]; // started tasks per type, for debugging; last = other

void osSpTaskLoad(OSTask* intp) {
    sSpLoadedTask = intp;
}

void osSpTaskStartGo(OSTask* tp) {
    u32 type = tp->t.type;

    if (tp != sSpLoadedTask) {
        gc_log("sp: osSpTaskStartGo(%p) without osSpTaskLoad (loaded %p)", (void*)tp, (void*)sSpLoadedTask);
    }
    sSpTaskCount[(type <= M_NJPEGTASK) ? type : (M_NJPEGTASK + 1)]++;

    // The task's output is complete before the done events: the frame is copied to its XFB by the time
    // the scheduler swaps to its framebuffer, the audio buffer is mixed by the time the audio manager
    // sees the task done.
    if (type == M_GFXTASK) {
        if (gc_gfx_enabled()) {
            gc_gfx_run_task((u32)tp->t.data_ptr);
        }
    } else if (type == M_AUDTASK) {
        u64 start = gc_time_ticks();

        Gc_AudRunTask(tp);
        __gcAiNoteTaskTime(gc_time_ticks() - start);
    } else if (type == M_NJPEGTASK) {
        Gc_NJpegRunTask(tp);
    }

    __gcPostEvent(OS_EVENT_SP);
    if (type == M_GFXTASK) {
        __gcPostEvent(OS_EVENT_DP);
    }
}

void osSpTaskYield(void) {
    // Tasks finish before they can be yielded
}

OSYieldResult osSpTaskYielded(OSTask* task) {
    return 0;
}

u32 __osSpGetStatus(void) {
    return IO_READ(SP_STATUS_REG);
}

void __osSpSetStatus(u32 data) {
    IO_WRITE(SP_STATUS_REG, data);
}

u32 osDpGetStatus(void) {
    return IO_READ(DPC_STATUS_REG);
}

void osDpSetStatus(u32 data) {
    IO_WRITE(DPC_STATUS_REG, data);
}

/**
 * libultra: __osSpSetPc(0), which fails with -1 if the RSP is still running. irqmgr.c calls it
 * 30 ms after PRENMI and only logs the result.
 */
s32 osAfterPreNMI(void) {
    if (!(__osSpGetStatus() & SP_STATUS_HALT)) {
        return -1;
    }
    IO_WRITE(SP_PC_REG, 0);
    return 0;
}

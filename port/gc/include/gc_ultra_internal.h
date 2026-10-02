/**
 * Internal interface shared by the files in port/gc/ultra/ (the libultra shim).
 * Compiled with the decomp's headers. Not for game code.
 */
#ifndef GC_ULTRA_INTERNAL_H
#define GC_ULTRA_INTERNAL_H

#include "ultra64.h"
#include "gc_bridge.h"
#include "gc_audio.h"

/** Map a libultra priority (0..255) to an LWP priority in [GC_PRIO_GAME_MIN, GC_PRIO_GAME_MAX]
 *  (0 stays GC_PRIO_IDLE). Order-preserving for the priorities the game uses. */
int __gcMapPriority(OSPri pri);

/** The OSThread of the calling game thread, or NULL for shim service threads. */
OSThread* __gcGetCurrentThread(void);

/** Send the message registered with osSetEventMesg() for `event`, without blocking.
 *  Safe to call from any thread, including shim service threads. Must NOT hold the OS lock. */
void __gcPostEvent(OSEvent event);

/** Send a message without blocking while already holding the OS lock (gc_os_lock).
 *  Returns 0 on success, -1 if the queue is full. */
s32 __gcSendMesgLocked(OSMesgQueue* mq, OSMesg msg, s32 jam);

/** Called by the VI service thread once per retrace (port/gc/ultra/vi.c). */
void __gcViRetrace(void);

/** Called from gc_ultra_boot() before bootproc(): start the timer and VI service threads. */
void __gcTimerInit(void);
void __gcViInit(void);

/** Write unsaved flash (save data) changes to the card now. Blocks on storage I/O (port/gc/ultra/flash.c). */
void __gcFlashFlush(void);

/** Software RSP tasks: the audio command list interpreter (Gc_AudRunTask, port/gc/audio, declared in
 *  gc_audio.h), which the audio manager thread runs itself (Gc_AudioMgrRunTask below), and the NJPEG
 *  decoder (port/gc/game/njpeg_cpu.c, gc_game.h), which osSpTaskStartGo() (sp.c) runs synchronously
 *  on the Sched thread; sp.c also runs any M_AUDTASK that reaches the scheduler. task_weak.c has weak
 *  no-op versions, so the link works before (or without) the real ones. */
void Gc_NJpegRunTask(OSTask* task);

/** Audio statistics (ai.c): run time of one software audio task, in timebase ticks (sp.c). */
void __gcAiNoteTaskTime(u64 ticks);

/** For the audio manager (TARGET_GC code in src/code/audio_thread_manager.c, which declares them
 *  itself), in ai.c: run an audio command list on the calling thread (Gc_AudRunTask, timed), and
 *  record the run time of one AudioThread_Update, in OSTime cycles. */
void Gc_AudioMgrRunTask(OSTask* task);
void Gc_AudioMgrUpdateTime(OSTime time);

/** Stop with a message naming an unimplemented libultra function. */
#define GC_TODO(name) gc_halt("GC_TODO: %s not implemented (%s:%d)", name, __FILE__, __LINE__)

#endif

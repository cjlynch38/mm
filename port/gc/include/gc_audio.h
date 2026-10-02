/**
 * Software audio microcode (port/gc/audio): runs the game's audio RSP tasks on the CPU.
 * Compiled with the decomp's headers.
 */
#ifndef GC_AUDIO_H
#define GC_AUDIO_H

#include "PR/sptask.h"

/**
 * Execute an M_AUDTASK OSTask as MM's audio microcode (aspMain) would: every command of the list at
 * task->t.data_ptr (task->t.data_size bytes, as AudioThread_Update fills them), reading and writing the
 * RAM the commands address. Synchronous; called by the audio manager thread (Gc_AudioMgrRunTask,
 * port/gc/ultra/ai.c) and by osSpTaskStartGo (port/gc/ultra/sp.c) for an M_AUDTASK sent to the
 * scheduler. Not reentrant: the microcode's DMEM and register state is global, as on the RSP, so only
 * one thread may run audio tasks.
 */
void Gc_AudRunTask(OSTask* task);

#endif

/**
 * Internal interface shared by the files in port/gc/ultra/ (the libultra shim).
 * Compiled with the decomp's headers. Not for game code.
 */
#ifndef GC_ULTRA_INTERNAL_H
#define GC_ULTRA_INTERNAL_H

#include "ultra64.h"
#include "gc_bridge.h"

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

/** Stop with a message naming an unimplemented libultra function. */
#define GC_TODO(name) gc_halt("GC_TODO: %s not implemented (%s:%d)", name, __FILE__, __LINE__)

#endif

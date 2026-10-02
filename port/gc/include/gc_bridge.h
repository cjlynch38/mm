/**
 * Plain-C interface between the two halves of the GameCube platform layer:
 *
 *   port/gc/ultra/  libultra API for the game, compiled with the decomp's headers
 *   port/gc/ogc/    implementation of this interface on libogc, compiled with libogc headers
 *
 * The decomp's headers and libogc's headers cannot share a translation unit (u32/s32 are
 * different types, and Mtx, Vtx and gu* clash), so this header uses only C built-in types.
 * Every function here is implemented in port/gc/ogc/ and is safe to call from any LWP thread
 * unless noted otherwise. None of them may be called from interrupt context.
 */
#ifndef GC_BRIDGE_H
#define GC_BRIDGE_H

/* ------------------------------------------------------------------------------------------ */
/* Logging and fatal errors                                                                     */
/* ------------------------------------------------------------------------------------------ */

/** printf-style log line to the on-screen console and to stdout (Dolphin log). Thread-safe. */
void gc_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Log a message, then stop all game progress forever (the console stays visible). */
void gc_halt(const char* fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

/* ------------------------------------------------------------------------------------------ */
/* Threads (libogc LWP)                                                                         */
/* ------------------------------------------------------------------------------------------ */

/* LWP priorities are 0 (idle) .. 127 (highest). Game threads are mapped into
 * [GC_PRIO_GAME_MIN, GC_PRIO_GAME_MAX]; shim service threads use GC_PRIO_SERVICE_*. */
#define GC_PRIO_IDLE 0
#define GC_PRIO_GAME_MIN 1
#define GC_PRIO_GAME_MAX 110
#define GC_PRIO_SERVICE_TIMER 125
#define GC_PRIO_SERVICE_VI 126

typedef unsigned int gc_thread_t;

/**
 * Create and start a thread running entry(arg). The bridge allocates a stack of stack_size
 * bytes (rounded up to 32) that is never freed. Returns 0 on success.
 */
int gc_thread_create(gc_thread_t* out, void (*entry)(void* arg), void* arg, unsigned int stack_size,
                     int prio);
gc_thread_t gc_thread_self(void);
void gc_thread_set_prio(gc_thread_t thread, int prio);
/** Yield to other ready threads of the same priority. */
void gc_thread_yield(void);
/** Suspend/resume a thread (osStopThread, or osStartThread on an already-started thread). */
void gc_thread_suspend(gc_thread_t thread);
void gc_thread_resume(gc_thread_t thread);
/** Terminate the calling thread. */
void gc_thread_exit(void) __attribute__((noreturn));

/* ------------------------------------------------------------------------------------------ */
/* Global OS lock: protects all libultra state (message queues, events, thread table)           */
/* ------------------------------------------------------------------------------------------ */

/** Non-recursive. Never call gc_os_lock() while already holding it. */
void gc_os_lock(void);
void gc_os_unlock(void);
/** Caller holds the lock: atomically release it, wait for gc_os_broadcast(), reacquire.
 *  Spurious wakeups are possible; always re-check the condition in a loop. */
void gc_os_wait(void);
/** Wake every thread blocked in gc_os_wait(). Call with the lock held. */
void gc_os_broadcast(void);

/* ------------------------------------------------------------------------------------------ */
/* Semaphores (for shim service threads such as the timer thread)                              */
/* ------------------------------------------------------------------------------------------ */

typedef unsigned int gc_sem_t;

int gc_sem_create(gc_sem_t* out, unsigned int initial_count);
void gc_sem_post(gc_sem_t sem);
void gc_sem_wait(gc_sem_t sem);
/** Wait at most `ticks` timebase ticks. Returns 0 if the semaphore was taken, 1 on timeout. */
int gc_sem_wait_ticks(gc_sem_t sem, unsigned long long ticks);

/* ------------------------------------------------------------------------------------------ */
/* Interrupt masking (osSetIntMask)                                                             */
/* ------------------------------------------------------------------------------------------ */

/** Disable external interrupts (and therefore preemption). Returns the previous level. */
unsigned int gc_irq_disable(void);
void gc_irq_restore(unsigned int level);

/* ------------------------------------------------------------------------------------------ */
/* Time                                                                                         */
/* ------------------------------------------------------------------------------------------ */

/** GameCube timebase frequency (bus clock / 4). */
#define GC_TB_HZ 40500000ull

/** Monotonic timebase ticks since an arbitrary epoch (libogc gettime()). */
unsigned long long gc_time_ticks(void);

/* ------------------------------------------------------------------------------------------ */
/* Video                                                                                        */
/* ------------------------------------------------------------------------------------------ */

/** Block until the next vertical retrace. Only the VI service thread calls this. */
void gc_video_wait_vsync(void);
void gc_video_set_black(int black);
/** Retrace rate of the current TV mode: 60 (NTSC/MPAL/progressive) or 50 (PAL). */
int gc_video_refresh_hz(void);

/* ------------------------------------------------------------------------------------------ */
/* Controllers                                                                                  */
/* ------------------------------------------------------------------------------------------ */

/* Button bits, same values as libogc PAD_BUTTON_* */
#define GC_PAD_LEFT 0x0001
#define GC_PAD_RIGHT 0x0002
#define GC_PAD_DOWN 0x0004
#define GC_PAD_UP 0x0008
#define GC_PAD_Z 0x0010
#define GC_PAD_R 0x0020
#define GC_PAD_L 0x0040
#define GC_PAD_A 0x0100
#define GC_PAD_B 0x0200
#define GC_PAD_X 0x0400
#define GC_PAD_Y 0x0800
#define GC_PAD_START 0x1000

typedef struct {
    unsigned short buttons; /* GC_PAD_* */
    signed char stickX;     /* main stick, about -100..100 */
    signed char stickY;
    signed char substickX; /* C-stick */
    signed char substickY;
    unsigned char triggerL; /* analog 0..255 */
    unsigned char triggerR;
    signed char connected; /* 1 if a controller is plugged in, 0 otherwise */
} gc_pad_t;

/** Poll all four ports (PAD_ScanPads) and fill pads[0..3]. */
void gc_pad_read(gc_pad_t pads[4]);
void gc_pad_rumble(int chan, int on);

/* ------------------------------------------------------------------------------------------ */
/* ROM (the user's baserom on SD)                                                               */
/* ------------------------------------------------------------------------------------------ */

/** Copy `size` bytes at physical ROM offset `rom_offset` into dst. Synchronous and thread-safe.
 *  Reads past the end of the ROM are zero-filled. Returns 0 on success. */
int gc_rom_read(unsigned int rom_offset, void* dst, unsigned int size);
unsigned int gc_rom_size(void);

/* ------------------------------------------------------------------------------------------ */
/* Save data (N64 flash image persisted on SD)                                                  */
/* ------------------------------------------------------------------------------------------ */

/** Load the save image into dst. Returns 0 if a save file existed and was read. */
int gc_save_load(void* dst, unsigned int size);
/** Persist the save image. Returns 0 on success. */
int gc_save_store(const void* src, unsigned int size);

/* ------------------------------------------------------------------------------------------ */
/* Memory and reset                                                                             */
/* ------------------------------------------------------------------------------------------ */

/** Permanent allocation for shim-owned buffers (stacks, caches). Never freed. NULL on failure. */
void* gc_mem_alloc(unsigned int size, unsigned int align);

/** Register a function called (from a normal thread, not an interrupt) when Reset is pressed. */
void gc_set_reset_callback(void (*callback)(void));

/* ------------------------------------------------------------------------------------------ */
/* Audio output (port/gc/ogc/bridge_audio.c): the GameCube audio interface (AI DMA)            */
/* ------------------------------------------------------------------------------------------ */

/** Start the AI at `rate` Hz (32000 or 48000). Called once, before any gc_audio_queue. */
void gc_audio_init(unsigned int rate);
/** Queue `bytes` of 16-bit big-endian interleaved stereo PCM for playback (copied; the caller's
 *  buffer may be reused immediately). Returns 0 on success, -1 if the queue is full (dropped). */
int gc_audio_queue(const void* samples, unsigned int bytes);
/** Bytes queued but not yet played (osAiGetLength semantics: what the DAC still has to play). */
unsigned int gc_audio_bytes_pending(void);

/* ------------------------------------------------------------------------------------------ */
/* Renderer (port/gc/gfx): runs the game's graphics tasks through GX                           */
/* ------------------------------------------------------------------------------------------ */

/** Called once by ogc/main.c after the console video init; the renderer takes over the display. */
void gc_gfx_init(void);
/** Nonzero if the renderer draws (zero in console-only builds: tasks are then skipped). */
int gc_gfx_enabled(void);
/** Run an F3DZEX2 graphics task synchronously (from osSpTaskStartGo, on the game's Sched thread).
 *  dlist is the task's data_ptr: the display list address as the game wrote it (KSEG0 or physical). */
void gc_gfx_run_task(unsigned int dlist);
/** Show the frame rendered into N64 framebuffer `framebuffer` (called by the VI service thread at the
 *  retrace where osViSwapBuffer's buffer becomes current). */
void gc_gfx_present(const void* framebuffer);

/* ------------------------------------------------------------------------------------------ */
/* Implemented on the ultra side (port/gc/ultra), called by port/gc/ogc/main.c                  */
/* ------------------------------------------------------------------------------------------ */

/** Start the game: libultra init, then bootproc(). Returns once the game threads are running. */
void gc_ultra_boot(void);

#endif

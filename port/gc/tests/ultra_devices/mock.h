/**
 * Control interface of the host mocks used by the ultra_devices test. Plain C types only, so it is
 * included both by mock_bridge.c (host headers) and by the files built with the decomp's headers.
 */
#ifndef ULTRA_DEVICES_MOCK_H
#define ULTRA_DEVICES_MOCK_H

#include "gc_bridge.h"

/* Failures and logging */
void mock_fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int mock_failures(void);
int mock_logs(void);
/** Run fn(arg); return 1 if it called gc_halt() (which then returns here), 0 otherwise. */
int mock_expect_halt(void (*fn)(void* arg), void* arg);

/* Threads and locks */
int mock_os_lock_held(void); /* by the calling thread */
void mock_sleep_ms(unsigned int ms);
unsigned int mock_now_ms(void); /* monotonic */
int mock_threads_created(void);
int mock_thread_prio(int index);
unsigned int mock_thread_stack(int index);

/* Video: release one gc_video_wait_vsync() and wait until the VI thread is blocked in it again */
void mock_vsync(void);
int mock_black_calls(void);
int mock_black_last(void);

/* Controllers */
void mock_set_pads(const gc_pad_t pads[4]);
int mock_pad_reads(void);
int mock_rumble(int chan); /* last state set, -1 if never */
int mock_rumble_calls(void);

/* ROM */
void mock_rom_set_write(int enable); /* 1: gc_rom_read fills dst with a pattern; 0: only records */
int mock_rom_reads(void);
unsigned int mock_rom_last_offset(void);
unsigned int mock_rom_last_size(void);
void* mock_rom_last_dst(void);
unsigned char mock_rom_byte(unsigned int offset);

/* Save data. MOCK_SAVE in the environment: unset = no save file (gc_save_load returns 1),
 * 1 = an existing save, e = the save cannot be read (gc_save_load returns -1) */
int mock_has_initial_save(void);
int mock_save_error(void);
unsigned char mock_initial_save_byte(unsigned int offset);
int mock_save_loads(void);
int mock_save_stores(void);          /* successful stores */
int mock_save_store_attempts(void);  /* all calls to gc_save_store, failed ones included */
void mock_save_fail_next(int count); /* the next `count` stores fail (return -1, nothing stored) */
const unsigned char* mock_saved_image(void);

/* libultra core mocks (mock_ultra.c) */
int mock_event_posts(unsigned int event);

#endif

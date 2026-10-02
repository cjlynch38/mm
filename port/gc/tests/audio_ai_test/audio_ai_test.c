/**
 * audio_ai_test: stand-alone test DOL for the AI output path of the bridge
 * (port/gc/ogc/bridge_audio.c), without the game.
 *
 * A producer paced by the vertical retrace feeds gc_audio_queue() the way the game's audio thread
 * does: one buffer per retrace, sized from gc_audio_bytes_pending() by the game's own loop
 * (AudioThread_Update, src/audio/lib/thread.c) with the extra backlog and the refills of
 * port/gc/ultra/ai.c. A random delay of up to TEST_JITTER_MS before each buffer stands in for the
 * graphics tasks that the higher-priority Sched thread renders in the game. The signal is a sine of
 * TONE_L_HZ on the left and TONE_R_HZ on the right, continuous across buffers, so analyze_wav.py can
 * check pitch, level and continuity in Dolphin's audio dump.
 *
 * Steps:
 *   1. Rate: queue TEST_RATE_FRAMES of tone and watch gc_audio_bytes_pending() drain for 100 ms. It
 *      must fall by 4 bytes per frame at 32 kHz (within 1%).
 *   2. Steady state: TEST_STEADY1_SECONDS of the paced producer. No underrun, no drop, and the backlog
 *      never runs out.
 *   3. Stall: no buffers for TEST_STALL_MS, which must cause exactly one underrun; then the producer
 *      refills the queue and runs for TEST_STEADY2_SECONDS more without another one.
 *   4. Overflow: queue without pacing until the ring is full. gc_audio_queue() must then refuse
 *      (return -1) instead of overwriting audio that has not played yet.
 * The summary line is "audio_ai_test: RESULT PASS" or "audio_ai_test: RESULT FAIL".
 */
#include <gccore.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "gc_ogc.h"

/* Test hooks in bridge_audio.c */
unsigned int gc_ogc_audio_underruns(void);
unsigned int gc_ogc_audio_dropped(void);

#define RATE 32000
#define FRAME_SIZE 4
#define TONE_L_HZ 440.0
#define TONE_R_HZ 1000.0
#define TONE_AMPLITUDE 8192.0 /* -12 dBFS */

/* The game's loop at 32 kHz and 60 Hz (heap.c: target = ALIGN16(rate / refreshRate), +-16) */
#define TARGET_FRAMES 544
#define MIN_FRAMES (TARGET_FRAMES - 16)
#define MAX_FRAMES (TARGET_FRAMES + 16)
/* port/gc/ultra/ai.c */
#define LATENCY_FRAMES 512
#define PRIME_FRAMES (LATENCY_FRAMES + 128)

#define TEST_RATE_FRAMES 6000
#define TEST_JITTER_MS 7
#define TEST_STEADY1_SECONDS 8
#define TEST_STALL_MS 200
#define TEST_STEADY2_SECONDS 4

static short sBuf[1024 * 2] __attribute__((aligned(32)));
static short sZero[PRIME_FRAMES * 2] __attribute__((aligned(32)));
static double sPhaseL;
static double sPhaseR;
static unsigned int sPrimes;
static unsigned int sQueueFailures;
static unsigned int sBacklogMin = ~0u;
static unsigned int sBacklogMax;
static int sPassed;
static int sFailed;

static void check(int ok, const char* what) {
    gc_log("audio_ai_test: %s: %s", ok ? "PASS" : "FAIL", what);
    if (ok) {
        sPassed++;
    } else {
        sFailed++;
    }
}

static unsigned int elapsed_us(u64 since) {
    return (unsigned int)ticks_to_microsecs(gettime() - since);
}

/** Fill sBuf with `frames` frames of the test tone, continuing from the previous buffer. */
static void make_tone(unsigned int frames) {
    unsigned int i;

    for (i = 0; i < frames; i++) {
        sBuf[i * 2 + 0] = (short)lrint(TONE_AMPLITUDE * sin(sPhaseL));
        sBuf[i * 2 + 1] = (short)lrint(TONE_AMPLITUDE * sin(sPhaseR));
        sPhaseL += 2.0 * M_PI * TONE_L_HZ / RATE;
        sPhaseR += 2.0 * M_PI * TONE_R_HZ / RATE;
    }
    sPhaseL = fmod(sPhaseL, 2.0 * M_PI);
    sPhaseR = fmod(sPhaseR, 2.0 * M_PI);
}

/** One audio update as the game and ai.c do it. Returns the backlog it saw, in frames. */
static unsigned int produce(void) {
    unsigned int backlog = gc_audio_bytes_pending() / FRAME_SIZE;
    unsigned int reported;
    int frames;

    if (backlog == 0) {
        gc_audio_queue(sZero, PRIME_FRAMES * FRAME_SIZE);
        sPrimes++;
    }
    reported = (backlog > LATENCY_FRAMES) ? (backlog - LATENCY_FRAMES) : 0;
    frames = (((TARGET_FRAMES - (int)reported) + 128) & ~0xF) + 16;
    if (frames < MIN_FRAMES) {
        frames = MIN_FRAMES;
    }
    if (frames > MAX_FRAMES) {
        frames = MAX_FRAMES;
    }

    make_tone(frames);
    if (gc_audio_queue(sBuf, frames * FRAME_SIZE) != 0) {
        sQueueFailures++;
    }
    return backlog;
}

/** Paced producer for `seconds`: one buffer per retrace, after a random delay. */
static void run_paced(unsigned int seconds, int recordBacklog) {
    u64 start = gettime();
    u64 t;
    unsigned int backlog;
    unsigned int delay;

    while (elapsed_us(start) < seconds * 1000000u) {
        VIDEO_WaitVSync();
        delay = (unsigned int)(rand() % (TEST_JITTER_MS * 1000));
        t = gettime();
        while (elapsed_us(t) < delay) {
        }
        backlog = produce();
        if (recordBacklog) {
            if (backlog < sBacklogMin) {
                sBacklogMin = backlog;
            }
            if (backlog > sBacklogMax) {
                sBacklogMax = backlog;
            }
        }
    }
}

static void test_rate(void) {
    unsigned int before;
    unsigned int after;
    unsigned int us;
    unsigned int rate;
    u64 start;
    char msg[160];

    make_tone(TEST_RATE_FRAMES / 2);
    gc_audio_queue(sBuf, (TEST_RATE_FRAMES / 2) * FRAME_SIZE);
    make_tone(TEST_RATE_FRAMES / 2);
    gc_audio_queue(sBuf, (TEST_RATE_FRAMES / 2) * FRAME_SIZE);

    // Let the first block start, then time the drain
    usleep(20000);
    start = gettime();
    before = gc_audio_bytes_pending();
    usleep(100000);
    after = gc_audio_bytes_pending();
    us = elapsed_us(start);

    rate = (unsigned int)(((unsigned long long)(before - after) * 1000000ull) / ((unsigned long long)us * FRAME_SIZE));
    snprintf(msg, sizeof(msg), "pending drains at %u frames/s (%u -> %u bytes in %u us), expected %u +-1%%", rate,
             before, after, us, RATE);
    check((before > after) && (rate > RATE * 99 / 100) && (rate < RATE * 101 / 100), msg);
}

int main(void) {
    char msg[160];
    unsigned int underruns;
    unsigned int dropped;
    unsigned int i;
    int full;

    gc_ogc_video_init();
    gc_ogc_init();
    gc_log("audio_ai_test: AI output test, %d Hz, tones %d Hz (left) and %d Hz (right)", RATE, (int)TONE_L_HZ,
           (int)TONE_R_HZ);
    srand(1234);

    gc_audio_init(RATE);
    check(gc_audio_bytes_pending() == 0, "nothing pending after gc_audio_init");
    usleep(50000);

    // 1. Rate
    test_rate();

    // 2. Steady state
    run_paced(TEST_STEADY1_SECONDS, 1);
    underruns = gc_ogc_audio_underruns();
    snprintf(msg, sizeof(msg), "%d s paced with up to %d ms jitter: %u underruns, %u refills, %u queue failures",
             TEST_STEADY1_SECONDS, TEST_JITTER_MS, underruns, sPrimes, sQueueFailures);
    check((underruns == 0) && (sPrimes == 0) && (sQueueFailures == 0), msg);
    snprintf(msg, sizeof(msg), "backlog stays in %u-%u frames (more than 0, less than the ring)", sBacklogMin,
             sBacklogMax);
    check((sBacklogMin > 0) && (sBacklogMax < 8192), msg);

    // 3. Stall, then recovery
    usleep(TEST_STALL_MS * 1000);
    run_paced(TEST_STEADY2_SECONDS, 0);
    underruns = gc_ogc_audio_underruns();
    snprintf(msg, sizeof(msg), "%d ms stall: %u underruns (expected 1), %u refills (expected 1), %u queue failures",
             TEST_STALL_MS, underruns, sPrimes, sQueueFailures);
    check((underruns == 1) && (sPrimes == 1) && (sQueueFailures == 0), msg);

    // 4. Overflow: the ring holds 256 ms, so 64 buffers of 560 frames (1.1 s) cannot all fit
    full = 0;
    dropped = gc_ogc_audio_dropped();
    for (i = 0; (i < 64) && !full; i++) {
        make_tone(MAX_FRAMES);
        full = (gc_audio_queue(sBuf, MAX_FRAMES * FRAME_SIZE) != 0);
    }
    snprintf(msg, sizeof(msg), "full ring refuses buffer %u (pending %u bytes, drop count +%u)", i,
             gc_audio_bytes_pending(), gc_ogc_audio_dropped() - dropped);
    check(full && (i > 4) && (gc_ogc_audio_dropped() == dropped + 1), msg);

    // Let the queued tone play out
    usleep(400000);
    check(gc_audio_bytes_pending() == 0, "everything played after the producer stopped");

    gc_log("audio_ai_test: %d passed, %d failed", sPassed, sFailed);
    gc_log("audio_ai_test: RESULT %s", (sFailed == 0) ? "PASS" : "FAIL");

    for (;;) {
        VIDEO_WaitVSync();
    }
}

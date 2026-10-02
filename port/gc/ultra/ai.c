/**
 * libultra Audio Interface API (osAiSetFrequency, osAiGetLength, osAiSetNextBuffer) on the
 * GameCube audio interface (gc_audio_*, port/gc/ogc/bridge_audio.c). Replaces both
 * src/libultra/io/ai*.c and the game's own src/audio/lib/aisetnextbuf.c.
 *
 * Every audio update (each retrace), AudioThread_Update (src/audio/lib/thread.c) hands over one
 * buffer of 16-bit big-endian interleaved stereo PCM (left, right), which the bridge copies into the
 * AI's queue, and sizes the next buffer from osAiGetLength(), the bytes the DAC still has to play:
 *     frames = clamp(((target - osAiGetLength() / 4 + 128) & ~15) + 16, target - 16, target + 16)
 * with target = ALIGN16(rate / 60) = 544 frames at 32 kHz. That loop settles where osAiGetLength()
 * reads 130-160 frames (about 4.5 ms) when the update runs. On N64 this is enough because the audio
 * thread runs promptly at each retrace. Here the update first runs the previous update's command
 * list on the CPU (Gc_AudioMgrRunTask, 1-5 ms), so its timing varies more, and other work can delay
 * it now and then. osAiGetLength() therefore reports the real backlog minus AI_LATENCY_FRAMES, so
 * the loop keeps that much more queued.
 *
 * Silence (AI_PRIME_FRAMES) is queued before the first buffer and again whenever the queue has run
 * dry, so the loop starts at its working point instead of creeping up to it at 16 frames per
 * update. Rates the AI cannot play (it has 32000 and 48000 Hz) are converted to the AI rate by
 * linear interpolation. MM only has one: 22050 Hz, audio spec 0x13, which no scene selects.
 *
 * GC_AUDIO=0 builds (gc_options.h) never submit audio tasks (R_AUDIOMGR_DEBUG_LEVEL 1, see
 * audio_thread_manager.c): the AI is never started and the buffers are dropped, as in M2.
 *
 * The file also runs the audio manager's command lists (Gc_AudioMgrRunTask) and keeps the audio
 * statistics logged every AI_STATS_UPDATES updates: buffer timing, the backlog, queue refills, and
 * the CPU time of the driver update and of the command list.
 */
#include "gc_ultra_internal.h"
#include "gc_options.h"
#include "stdbool.h"

#define AI_FRAME_SIZE 4 // bytes per stereo frame (2 x s16)

// The AI's two rates: nominal (gc_audio_init) and real (54 MHz / 1686 and 54 MHz / 1124; Dolphin
// writes its dumps of 32 kHz output at 32028 Hz)
#define AI_RATE_32K 32000
#define AI_RATE_32K_REAL 32028
#define AI_RATE_48K 48000
#define AI_RATE_48K_REAL 48043

// Extra backlog the game is made to keep, in frames at the game's rate: 16 ms at 32 kHz. With the loop's
// own 130-160 frames, an update can come about 20 ms late (a skipped retrace, nearly) before the AI runs dry.
#define AI_LATENCY_FRAMES 512
// Silence queued at the start and after an underrun: the latency plus the loop's own working point
#define AI_PRIME_FRAMES (AI_LATENCY_FRAMES + 128)
// Statistics interval (10 s at 60 updates per second)
#define AI_STATS_UPDATES 600

// Rate conversion output: the game's largest buffer (AIBUF_LEN / 2 = 704 frames) at 22050 Hz becomes
// about 1022 frames at 32000 Hz
#define AI_RESAMPLE_MAX_FRAMES 2048
#define AI_SILENCE_FRAMES 128

static s32 sAiStarted;  // osAiSetFrequency() has started the AI
static u32 sAiNominal;  // the AI's nominal rate (32000 or 48000)
static u32 sAiOutRate;  // the rate it really plays at
static u32 sAiGameRate; // rate the game produces
static u32 sAiStep;     // input frames per output frame (16.16) when converting, 0 when not
static u32 sAiPhase;    // position of the next output frame (16.16); 0 is sAiPrevFrame
static s16 sAiPrevFrame[2];
static s16 sAiResampleBuf[AI_RESAMPLE_MAX_FRAMES * 2];
static s16 sAiSilence[AI_SILENCE_FRAMES * 2];

static u64 sAiLastBufferTime; // gc_time_ticks() of the last buffer
static u32 sAiRefillCount;    // queue refills since the start (the first is the initial one)

// Statistics since the last report
static u32 sAiUpdates;
static u32 sAiLate;       // buffers more than 1.5 retraces after the previous one
static u32 sAiMaxGapUs;   // longest time between two buffers
static u32 sAiPrimes;
static u32 sAiDropped;
static u32 sAiBacklogMin = 0xFFFFFFFF;
static u32 sAiBacklogMax;
static u32 sAiDriverUpdates;
static u64 sAiDriverUs;
static u32 sAiDriverMaxUs;
static u32 sAiTasks;
static u64 sAiTaskUs;
static u32 sAiTaskMaxUs;

/**
 * Converts `inFrames` frames from the game's rate to the AI's. Input position 0 is the last frame of
 * the previous buffer, so the output is continuous across buffers. Returns the frames written.
 */
static u32 Ai_Resample(const s16* in, u32 inFrames, s16* out, u32 maxOutFrames) {
    u32 pos = sAiPhase;
    u32 end = inFrames << 16;
    u32 outFrames = 0;
    u32 i;
    s32 frac;
    const s16* a;
    const s16* b;

    while ((pos < end) && (outFrames < maxOutFrames)) {
        i = pos >> 16;
        frac = (pos & 0xFFFF) >> 1; // 15 bits, so (b - a) * frac fits in an s32
        a = (i == 0) ? sAiPrevFrame : &in[(i - 1) * 2];
        b = &in[i * 2];
        out[outFrames * 2 + 0] = a[0] + (((b[0] - a[0]) * frac) >> 15);
        out[outFrames * 2 + 1] = a[1] + (((b[1] - a[1]) * frac) >> 15);
        outFrames++;
        pos += sAiStep;
    }

    sAiPhase = (pos > end) ? (pos - end) : 0;
    sAiPrevFrame[0] = in[(inFrames - 1) * 2 + 0];
    sAiPrevFrame[1] = in[(inFrames - 1) * 2 + 1];
    return outFrames;
}

/** Queue `frames` frames of silence at the AI's rate. */
static void Ai_QueueSilence(u32 frames) {
    u32 n;

    while (frames != 0) {
        n = (frames < AI_SILENCE_FRAMES) ? frames : AI_SILENCE_FRAMES;
        gc_audio_queue(sAiSilence, n * AI_FRAME_SIZE);
        frames -= n;
    }
}

static void Ai_LogStats(void) {
    u32 updates = sAiDriverUpdates;
    u32 tasks = sAiTasks;

    gc_log("ai: %u buffers (%u late, max gap %u us), %u refills, %u dropped, backlog %u-%u frames; "
           "driver update %u us avg (max %u), audio task %u us avg (max %u, %u tasks)",
           (unsigned)sAiUpdates, (unsigned)sAiLate, (unsigned)sAiMaxGapUs, (unsigned)sAiPrimes,
           (unsigned)sAiDropped, (unsigned)sAiBacklogMin, (unsigned)sAiBacklogMax,
           (unsigned)((updates != 0) ? (sAiDriverUs / updates) : 0), (unsigned)sAiDriverMaxUs,
           (unsigned)((tasks != 0) ? (sAiTaskUs / tasks) : 0), (unsigned)sAiTaskMaxUs, (unsigned)tasks);

    sAiUpdates = 0;
    sAiLate = 0;
    sAiMaxGapUs = 0;
    sAiPrimes = 0;
    sAiDropped = 0;
    sAiBacklogMin = 0xFFFFFFFF;
    sAiBacklogMax = 0;
    sAiDriverUpdates = 0;
    sAiDriverUs = 0;
    sAiDriverMaxUs = 0;
    sAiTasks = 0;
    sAiTaskUs = 0;
    sAiTaskMaxUs = 0;
}

/**
 * The first call starts the AI: at 48 kHz if that is what the game asks for, otherwise at 32 kHz.
 * Later rates other than the AI's nominal one are converted. As on N64, the result is the rate the
 * game's samples really play at: the AI's real rate (32028 Hz for 32000; the N64 gives 32006 on
 * NTSC), or the requested rate when it is converted. The game only uses it for a tempo constant.
 */
s32 osAiSetFrequency(u32 frequency) {
    if ((frequency < 1000) || (frequency > 96000)) {
        return -1;
    }
    if (!GC_AUDIO) {
        return frequency;
    }

    if (!sAiStarted) {
        sAiNominal = (frequency == AI_RATE_48K) ? AI_RATE_48K : AI_RATE_32K;
        sAiOutRate = (sAiNominal == AI_RATE_48K) ? AI_RATE_48K_REAL : AI_RATE_32K_REAL;
        gc_audio_init(sAiNominal);
        sAiStarted = true;
    }
    if (frequency != sAiGameRate) {
        sAiGameRate = frequency;
        sAiStep = (frequency == sAiNominal) ? 0 : (u32)(((u64)frequency << 16) / sAiOutRate);
        sAiPhase = 0;
        sAiPrevFrame[0] = sAiPrevFrame[1] = 0;
        if (sAiStep != 0) {
            gc_log("ai: game rate %u Hz, converted to the AI's %u Hz", (unsigned)frequency, (unsigned)sAiOutRate);
        }
    }
    return (sAiStep == 0) ? sAiOutRate : frequency;
}

/** Bytes (at the game's rate) still to play, minus the extra backlog the game is made to keep. */
u32 osAiGetLength(void) {
    u32 frames;

    if (!sAiStarted) {
        return 0;
    }

    frames = gc_audio_bytes_pending() / AI_FRAME_SIZE;
    if (sAiStep != 0) {
        frames = (u32)(((u64)frames * sAiGameRate) / sAiOutRate);
    }
    if (frames < sAiBacklogMin) {
        sAiBacklogMin = frames;
    }
    if (frames > sAiBacklogMax) {
        sAiBacklogMax = frames;
    }

    frames = (frames > AI_LATENCY_FRAMES) ? (frames - AI_LATENCY_FRAMES) : 0;
    return frames * AI_FRAME_SIZE;
}

/** Copy the buffer to the AI queue. Returns 0, or -1 if there was no room (the buffer is dropped). */
s32 osAiSetNextBuffer(void* buf, u32 size) {
    u32 frames = size / AI_FRAME_SIZE;
    u64 now = gc_time_ticks();
    u32 gapUs = (sAiLastBufferTime != 0) ? (u32)(((now - sAiLastBufferTime) * 1000000) / GC_TB_HZ) : 0;
    s32 ret;

    if (!sAiStarted || (frames == 0)) {
        return 0;
    }

    sAiLastBufferTime = now;
    if (gapUs > sAiMaxGapUs) {
        sAiMaxGapUs = gapUs;
    }
    if (gapUs > 1500000 / 60) {
        sAiLate++;
    }
    if (++sAiUpdates >= AI_STATS_UPDATES) {
        Ai_LogStats();
    }

    // Nothing left to play: the AI is already playing silence, so more of it costs nothing
    if (gc_audio_bytes_pending() == 0) {
        Ai_QueueSilence((u32)(((u64)AI_PRIME_FRAMES * sAiOutRate) / sAiGameRate));
        sAiPrimes++;
        if (sAiRefillCount++ != 0) {
            gc_log("ai: the queue ran dry (%u us since the previous buffer), refilled with silence", (unsigned)gapUs);
        }
    }

    if (sAiStep == 0) {
        ret = gc_audio_queue(buf, frames * AI_FRAME_SIZE);
    } else {
        frames = Ai_Resample(buf, frames, sAiResampleBuf, AI_RESAMPLE_MAX_FRAMES);
        ret = gc_audio_queue(sAiResampleBuf, frames * AI_FRAME_SIZE);
    }
    if (ret != 0) {
        sAiDropped++;
    }
    return ret;
}

void __gcAiNoteTaskTime(u64 ticks) {
    u32 us = (u32)((ticks * 1000000) / GC_TB_HZ);

    sAiTasks++;
    sAiTaskUs += us;
    if (us > sAiTaskMaxUs) {
        sAiTaskMaxUs = us;
    }
}

void Gc_AudioMgrRunTask(OSTask* task) {
    u64 start = gc_time_ticks();

    Gc_AudRunTask(task);
    __gcAiNoteTaskTime(gc_time_ticks() - start);
}

void Gc_AudioMgrUpdateTime(OSTime time) {
    u32 us = (u32)OS_CYCLES_TO_USEC(time);

    sAiDriverUpdates++;
    sAiDriverUs += us;
    if (us > sAiDriverMaxUs) {
        sAiDriverMaxUs = us;
    }
}

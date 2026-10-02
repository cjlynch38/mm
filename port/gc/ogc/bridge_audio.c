/**
 * Audio output (gc_bridge.h, "Audio output") on the GameCube audio interface (AI) DMA.
 *
 * The game produces 16-bit big-endian interleaved stereo PCM, one buffer per audio update. The AI
 * DMA reads the same big-endian 16-bit samples (the N64 and the GameCube are both big-endian), but
 * each frame right channel first, where the N64 has left first. gc_audio_queue() copies each buffer
 * into a ring in MEM1, swapping the two samples of every frame, and flushes it from the data cache;
 * the AI DMA plays the ring in blocks of at most AUD_BLOCK_MAX bytes. In 32 kHz mode the AI really
 * runs at 54 MHz / 1686 = 32028 Hz (Dolphin writes its audio dumps at that rate).
 *
 * The AI DMA address and length registers are double-buffered. The AID interrupt fires when the
 * block programmed last has been latched and starts playing; its callback then programs the block
 * that follows. So at any time one block is playing ("current"), one is programmed ("next"), and
 * the bytes after the next block wait in the ring ("queued"). When nothing is queued, the callback
 * programs a short block of silence instead and counts an underrun; the next interrupt picks the
 * ring up again. The DMA therefore runs without a break from gc_audio_init() on.
 *
 * Ring positions are free-running byte counters (unsigned wrap-around); a position's offset in the
 * ring is the counter modulo AUD_RING_SIZE. Blocks start and end on 32-byte boundaries, as the DMA
 * requires. The first AUD_BLOCK_MAX bytes of the ring are mirrored after its end, so a block can
 * run past the end of the ring in one piece: a block cut short at the end could be as short as
 * 32 bytes (a quarter of a millisecond), too little time to service the interrupt that programs the
 * block after it. Short blocks then only happen while the queue is about to run dry.
 *
 * The callback runs in interrupt context and only does this bookkeeping; the copy and the cache
 * flush happen in gc_audio_queue(), which is meant for one producer thread (the audio manager).
 * Thread-side reads and updates of the shared state run with interrupts disabled.
 */
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include "gc_ogc.h"

/* Ring: 32 KB = 8192 stereo frames = 256 ms at 32 kHz. The game keeps about 40 ms queued. A multiple
 * of the frame size, so no frame straddles the end of the ring. */
#define AUD_RING_SIZE 0x8000u
/* Largest DMA block: 1 KB = 256 frames = 8 ms at 32 kHz, so about 125 interrupts per second */
#define AUD_BLOCK_MAX 0x400u
/* Block of silence played while the ring is empty: 64 frames = 2 ms */
#define AUD_SILENCE_SIZE 0x100u
/* DMA address and length granularity */
#define AUD_DMA_ALIGN 32u
/* Bytes per stereo frame */
#define AUD_FRAME_SIZE 4u
/* Statistics are logged after every this many seconds of queued audio */
#define AUD_STATS_SECONDS 10u

/* DSP control register: bit 3 is set while an AI DMA interrupt is pending (not yet acknowledged) */
#define AUD_DSPCR (*(volatile unsigned short*)0xCC00500A)
#define AUD_DSPCR_AIDINT 0x0008

#define AUD_MIN(a, b) (((a) < (b)) ? (a) : (b))

static unsigned char* sRing;
static unsigned char* sSilence;
static unsigned int sRate;
static int sActive; /* gc_audio_init() has started the DMA */

/* Shared with the interrupt callback */
static volatile unsigned int sWritePos;  /* bytes queued by gc_audio_queue() so far */
static volatile unsigned int sReadPos;   /* bytes handed to the DMA so far */
static volatile unsigned int sCurStart;  /* ring position of the playing block */
static volatile unsigned int sCurLen;    /* its length in bytes, 0 for silence */
static volatile unsigned int sNextStart; /* ring position of the programmed block */
static volatile unsigned int sNextLen;   /* its length in bytes, 0 for silence */
static volatile int sStarted;            /* the DMA has played ring data at least once */
static volatile int sStarved;            /* the last block programmed was silence */
static volatile unsigned int sUnderruns; /* times the ring ran dry after playing data */
static volatile unsigned int sSilenceBlocks;
static volatile unsigned int sInterrupts;

/* Thread side only */
static unsigned int sQueued;  /* buffers accepted */
static unsigned int sDropped; /* buffers rejected because the ring was full */
static unsigned int sStatsBytes;
static unsigned int sStatsUnderruns;
static unsigned int sStatsSilenceBlocks;
static unsigned int sStatsInterrupts;
static unsigned int sStatsQueued;
static unsigned int sStatsDropped;
static unsigned int sPendingMin = ~0u;
static unsigned int sPendingMax;

/**
 * AID interrupt: the block programmed last has started playing. Program the one after it: up to
 * AUD_BLOCK_MAX bytes of queued data (reaching into the mirror after the ring's end if need be), or
 * silence.
 */
static void AudioDmaCallback(void) {
    unsigned int avail;
    unsigned int len;

    sInterrupts++;
    sCurStart = sNextStart;
    sCurLen = sNextLen;

    avail = (sWritePos - sReadPos) & ~(AUD_DMA_ALIGN - 1);
    if (avail != 0) {
        len = AUD_MIN(avail, AUD_BLOCK_MAX);
        AUDIO_InitDMA((unsigned int)(sRing + (sReadPos % AUD_RING_SIZE)), len);
        sNextStart = sReadPos;
        sNextLen = len;
        sReadPos += len;
        sStarted = 1;
        sStarved = 0;
    } else {
        AUDIO_InitDMA((unsigned int)sSilence, AUD_SILENCE_SIZE);
        sNextStart = sReadPos;
        sNextLen = 0;
        if (sStarted && !sStarved) {
            sUnderruns++;
        }
        sStarved = 1;
        sSilenceBlocks++;
    }
}

void gc_audio_init(unsigned int rate) {
    if (sActive) {
        if (rate != sRate) {
            gc_log("audio: gc_audio_init(%u) ignored, the AI already runs at %u Hz", rate, sRate);
        }
        return;
    }
    if ((rate != 32000) && (rate != 48000)) {
        gc_log("audio: %u Hz is not an AI rate, using 32000 Hz", rate);
        rate = 32000;
    }

    sRing = gc_mem_alloc(AUD_RING_SIZE + AUD_BLOCK_MAX, AUD_DMA_ALIGN);
    sSilence = gc_mem_alloc(AUD_SILENCE_SIZE, AUD_DMA_ALIGN);
    if ((sRing == NULL) || (sSilence == NULL)) {
        gc_log("audio: no memory for the output ring, sound is off");
        return;
    }
    memset(sSilence, 0, AUD_SILENCE_SIZE);
    DCFlushRange(sSilence, AUD_SILENCE_SIZE);

    AUDIO_Init(NULL);
    AUDIO_SetDSPSampleRate((rate == 48000) ? AI_SAMPLERATE_48KHZ : AI_SAMPLERATE_32KHZ);
    sRate = rate;

    // Start on silence; the first interrupt (as soon as it is latched) programs what follows
    AUDIO_RegisterDMACallback(AudioDmaCallback);
    AUDIO_InitDMA((unsigned int)sSilence, AUD_SILENCE_SIZE);
    AUDIO_StartDMA();
    sActive = 1;

    gc_log("audio: AI DMA at %u Hz, %u KB ring, blocks of up to %u bytes", rate, AUD_RING_SIZE / 1024,
           AUD_BLOCK_MAX);
}

unsigned int gc_audio_bytes_pending(void) {
    unsigned int level;
    unsigned int left;
    unsigned int latched;
    unsigned int pending;

    if (!sActive) {
        return 0;
    }

    _CPU_ISR_Disable(level);
    // The next block can be latched between reading the counter and reading the flag. With interrupts
    // off the flag only goes from clear to set, so reading it before and after the counter tells which
    // block the counter belongs to (at most one retry).
    do {
        latched = AUD_DSPCR & AUD_DSPCR_AIDINT;
        left = AUDIO_GetDMABytesLeft();
    } while (latched != (AUD_DSPCR & AUD_DSPCR_AIDINT));
    if (latched) {
        // The programmed block has already been latched (its interrupt is still pending): the
        // hardware counter belongs to it, and the bookkeeping has not moved on yet
        pending = (sNextLen != 0) ? AUD_MIN(left, sNextLen) : 0;
    } else {
        pending = ((sCurLen != 0) ? AUD_MIN(left, sCurLen) : 0) + sNextLen;
    }
    pending += sWritePos - sReadPos;
    _CPU_ISR_Restore(level);

    return pending;
}

/**
 * Store whole frames at ring offset `offset` (the range must not cross the end of the ring),
 * converting from the game's left, right order to the AI's right, left order. Updates the mirror
 * after the end of the ring and flushes everything written from the data cache.
 */
static void AudioStoreFrames(unsigned int offset, const unsigned char* src, unsigned int bytes) {
    unsigned short* d = (unsigned short*)(sRing + offset);
    const unsigned short* s = (const unsigned short*)src;
    unsigned int n = bytes / AUD_FRAME_SIZE;
    unsigned int mirrored;

    while (n-- != 0) {
        d[0] = s[1];
        d[1] = s[0];
        d += 2;
        s += 2;
    }
    DCFlushRange(sRing + offset, bytes);

    if (offset < AUD_BLOCK_MAX) {
        mirrored = AUD_MIN(bytes, AUD_BLOCK_MAX - offset);
        memcpy(sRing + AUD_RING_SIZE + offset, sRing + offset, mirrored);
        DCFlushRange(sRing + AUD_RING_SIZE + offset, mirrored);
    }
}

/** Log a statistics line covering everything since the last one. */
static void AudioLogStats(void) {
    unsigned int underruns = sUnderruns;
    unsigned int silence = sSilenceBlocks;
    unsigned int interrupts = sInterrupts;
    unsigned int silenceMs = ((silence - sStatsSilenceBlocks) * (AUD_SILENCE_SIZE / AUD_FRAME_SIZE) * 1000u) / sRate;

    gc_log("audio: %u buffers (%u ms), %u dropped, %u underruns (%u ms of silence), backlog %u-%u frames, %u irqs",
           sQueued - sStatsQueued, (sStatsBytes / AUD_FRAME_SIZE) * 1000u / sRate, sDropped - sStatsDropped,
           underruns - sStatsUnderruns, silenceMs, sPendingMin / AUD_FRAME_SIZE, sPendingMax / AUD_FRAME_SIZE,
           interrupts - sStatsInterrupts);

    sStatsBytes = 0;
    sStatsUnderruns = underruns;
    sStatsSilenceBlocks = silence;
    sStatsInterrupts = interrupts;
    sStatsQueued = sQueued;
    sStatsDropped = sDropped;
    sPendingMin = ~0u;
    sPendingMax = 0;
}

int gc_audio_queue(const void* samples, unsigned int bytes) {
    unsigned int level;
    unsigned int used;
    unsigned int pending;
    unsigned int offset;
    unsigned int first;

    if (!sActive) {
        return -1;
    }
    bytes &= ~(AUD_FRAME_SIZE - 1);
    if (bytes == 0) {
        return 0;
    }

    pending = gc_audio_bytes_pending();
    if (pending < sPendingMin) {
        sPendingMin = pending;
    }
    if (pending > sPendingMax) {
        sPendingMax = pending;
    }

    // The playing block is the oldest data the DMA still reads; everything from there to the write
    // position is in use. (While silence plays, sCurStart equals the next block's start.)
    _CPU_ISR_Disable(level);
    used = sWritePos - sCurStart;
    _CPU_ISR_Restore(level);

    if (bytes > AUD_RING_SIZE - used) {
        sDropped++;
        return -1;
    }

    // Only this function moves sWritePos, and the DMA never reads at or after it
    offset = sWritePos % AUD_RING_SIZE;
    first = AUD_MIN(bytes, AUD_RING_SIZE - offset);
    AudioStoreFrames(offset, samples, first);
    if (first < bytes) {
        AudioStoreFrames(0, (const unsigned char*)samples + first, bytes - first);
    }

    _CPU_ISR_Disable(level);
    sWritePos += bytes;
    _CPU_ISR_Restore(level);

    sQueued++;
    sStatsBytes += bytes;
    if (sStatsBytes >= sRate * AUD_FRAME_SIZE * AUD_STATS_SECONDS) {
        AudioLogStats();
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------ */
/* For tests (port/gc/tests/audio_ai_test)                                                     */
/* ------------------------------------------------------------------------------------------ */

/** Underruns (the ring ran dry after playing data) since gc_audio_init(). */
unsigned int gc_ogc_audio_underruns(void) {
    return sUnderruns;
}

/** Buffers gc_audio_queue() rejected because the ring was full. */
unsigned int gc_ogc_audio_dropped(void) {
    return sDropped;
}

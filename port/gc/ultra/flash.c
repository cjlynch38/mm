/**
 * libultra 1 Mbit FlashRAM driver (osFlash*) for the GameCube, replacing src/code/osFlash.c.
 *
 * The flash chip is a 128 KiB RAM image (erased bytes read 0xFF) loaded from SD with
 * gc_save_load() by osFlashInit(). It reports itself as a Macronix "C" part
 * (0x11118001 / FLASH_VERSION_MX_C), which SysFlashrom_CheckFlashType() accepts.
 *
 * The functions follow the real driver's contract as seen by sys_flashrom.c:
 *   - osFlashReadArray / osFlashWriteBuffer: the data moves immediately and the I/O message is sent
 *     NOBLOCK to `mq` exactly once, like the final PI DMA of the real driver (which consumes the
 *     messages of any intermediate DMAs itself).
 *   - osFlashWriteArray / osFlashSectorErase / osFlashAllErase are synchronous and send nothing;
 *     the real ones poll the chip with private timers and finish with osFlashClearStatus().
 *   - Programming a page ANDs the page buffer into the page, as NOR flash can only clear bits
 *     (sys_flashrom.c always erases first, so this is the same as a copy for the game).
 *   - An erase sector is 128 pages (16 KiB); sys_flashrom.c only writes from sector boundaries.
 *
 * Changes reach SD from a low-priority writer thread once the flash has not been modified for
 * one second, so game threads never wait for an SD write; a failed SD write is retried up to
 * GC_FLASH_PERSIST_RETRIES times, a second apart. Reads and writes of the image are
 * serialised by a private mutex, never the OS lock, because the writer holds it during a 128 KiB copy.
 * If a save exists but cannot be read, nothing is ever written back, so it is not replaced by an
 * erased image.
 */
#include "gc_ultra_internal.h"
#include "PR/os_internal_flash.h"
#include "stdbool.h"
#include "string.h"

#define GC_FLASH_TYPE_MAGIC 0x11118001 // FLASH_TYPE_MAGIC in sys_flashrom.h
#define GC_FLASH_VENDOR FLASH_VERSION_MX_C
#define GC_FLASH_NUM_PAGES (FLASH_SIZE / FLASH_BLOCK_SIZE)
#define GC_FLASH_SECTOR_PAGES 128

// Status register bits the real driver checks after an operation
#define GC_FLASH_STATUS_WRITE_DONE 0x04
#define GC_FLASH_STATUS_ERASE_DONE 0x08

#define GC_FLASH_PERSIST_DELAY_TICKS (GC_TB_HZ * 1) // persist 1 s after the last change
#define GC_FLASH_PERSIST_RETRIES 3                    // failed SD writes retried this often, 1 s apart
#define GC_FLASH_WRITER_STACK_SIZE 0x8000             // libfat and the SD driver run on this stack

void __gcFlashFlush(void);

static OSPiHandle sFlashHandle;
static s32 sFlashInitialized;
static u8* sFlashImage;  // the chip contents
static u8* sFlashShadow; // copy handed to gc_save_store()
static u8 sFlashPageBuf[FLASH_BLOCK_SIZE];
static u8 sFlashStatus;
static s32 sFlashDirty;         // image changed since the last copy to sFlashShadow
static s32 sFlashPersistBroken; // the save on SD could not be read: never overwrite it
static gc_sem_t sFlashMutex;        // sFlashImage, sFlashPageBuf, sFlashStatus, sFlashDirty
static gc_sem_t sFlashPersistMutex; // sFlashShadow and gc_save_store()
static gc_sem_t sFlashChangeSem;    // posted on every change, wakes the writer thread
static gc_thread_t sFlashWriterThread;

static void FlashLock(void) {
    gc_sem_wait(sFlashMutex);
}

static void FlashUnlock(void) {
    gc_sem_post(sFlashMutex);
}

/** Store the image if it has unsaved changes. Returns true if a store failed (still unsaved). */
static s32 FlashPersist(void) {
    s32 dirty;
    s32 failed = false;

    gc_sem_wait(sFlashPersistMutex);

    FlashLock();
    dirty = sFlashDirty && !sFlashPersistBroken;
    if (dirty) {
        memcpy(sFlashShadow, sFlashImage, FLASH_SIZE);
        sFlashDirty = false;
    }
    FlashUnlock();

    if (dirty && (gc_save_store(sFlashShadow, FLASH_SIZE) != 0)) {
        FlashLock();
        sFlashDirty = true;
        FlashUnlock();
        failed = true;
    }

    gc_sem_post(sFlashPersistMutex);
    return failed;
}

static void FlashNotifyWriter(void) {
    gc_sem_post(sFlashChangeSem);
}

static void FlashWriterMain(void* arg) {
    s32 retries = 0;

    while (true) {
        gc_sem_wait(sFlashChangeSem);
        // A save is hundreds of page writes: wait until the flash has been quiet for a while
        while (gc_sem_wait_ticks(sFlashChangeSem, GC_FLASH_PERSIST_DELAY_TICKS) == 0) {}

        if (!FlashPersist()) {
            retries = 0;
        } else if (retries < GC_FLASH_PERSIST_RETRIES) {
            // SD writes can fail transiently. Without a retry the save would only reach SD with the
            // game's next save, and would be lost if the console were switched off before that.
            retries++;
            gc_log("flash: writing the save to SD failed; retry %d of %d in 1 s", (int)retries,
                   GC_FLASH_PERSIST_RETRIES);
            FlashNotifyWriter();
        } else {
            gc_log("flash: writing the save to SD failed; retrying after the next change");
            retries = 0;
        }
    }
}

/** Record a modification. Caller holds the flash mutex; call FlashNotifyWriter() after unlocking. */
static void FlashMarkDirty(void) {
    sFlashDirty = true;
}

/**
 * Write the image to SD now if it has unsaved changes. Blocks on SD I/O; meant for the reset path,
 * so a save made less than a second before Reset is not lost.
 */
void __gcFlashFlush(void) {
    if (sFlashInitialized && FlashPersist()) {
        gc_log("flash: writing the save to SD failed");
    }
}

OSPiHandle* osFlashInit(void) {
    s32 loadResult;

    if (sFlashInitialized) {
        return &sFlashHandle;
    }

    sFlashHandle.type = DEVICE_TYPE_FLASH;
    sFlashHandle.baseAddress = PHYS_TO_K1(FLASH_START_ADDR);
    sFlashHandle.latency = FLASH_LATENCY;
    sFlashHandle.pulse = FLASH_PULSE;
    sFlashHandle.pageSize = FLASH_PAGE_SIZE;
    sFlashHandle.relDuration = FLASH_REL_DURATION;
    sFlashHandle.domain = PI_DOMAIN2;
    sFlashHandle.speed = 0;
    bzero(&sFlashHandle.transferInfo, sizeof(__OSTranxInfo));

    sFlashImage = gc_mem_alloc(FLASH_SIZE, 32);
    sFlashShadow = gc_mem_alloc(FLASH_SIZE, 32);
    if ((sFlashImage == NULL) || (sFlashShadow == NULL) || (gc_sem_create(&sFlashMutex, 1) != 0) ||
        (gc_sem_create(&sFlashPersistMutex, 1) != 0) || (gc_sem_create(&sFlashChangeSem, 0) != 0)) {
        gc_halt("flash: out of memory or semaphores");
    }

    loadResult = gc_save_load(sFlashImage, FLASH_SIZE);
    if (loadResult == 0) {
        gc_log("flash: save loaded from SD");
    } else {
        memset(sFlashImage, 0xFF, FLASH_SIZE);
        if (loadResult > 0) {
            gc_log("flash: no save on SD, starting with an erased flash");
        } else {
            // The bridge returns > 0 for "no save file" and < 0 for errors (no SD card, read
            // failure). Persisting an erased image would replace a save that may still be intact.
            sFlashPersistBroken = true;
            gc_log("flash: could not load the save; saves made in this session will NOT be written to SD");
        }
    }
    sFlashStatus = 0;
    sFlashDirty = false;

    if (gc_thread_create(&sFlashWriterThread, FlashWriterMain, NULL, GC_FLASH_WRITER_STACK_SIZE, GC_PRIO_GAME_MIN) !=
        0) {
        gc_halt("flash: cannot create the save writer thread");
    }

    sFlashInitialized = true;
    return &sFlashHandle;
}

/** The game always calls osFlashInit() first; this only guards against a call before it. */
static void FlashEnsureInit(void) {
    if (!sFlashInitialized) {
        osFlashInit();
    }
}

void osFlashReadStatus(u8* flashStatus) {
    FlashEnsureInit();
    FlashLock();
    *flashStatus = sFlashStatus;
    FlashUnlock();
}

void osFlashReadId(u32* flashType, u32* flashVendor) {
    *flashType = GC_FLASH_TYPE_MAGIC;
    *flashVendor = GC_FLASH_VENDOR;
}

void osFlashClearStatus(void) {
    FlashEnsureInit();
    FlashLock();
    sFlashStatus = 0;
    FlashUnlock();
}

/** Erase pages [firstPage, firstPage + numPages) and return the driver's result. */
static s32 FlashErase(u32 firstPage, u32 numPages) {
    u8 status;

    FlashEnsureInit();
    if (firstPage >= GC_FLASH_NUM_PAGES) {
        gc_log("flash: erase of page 0x%lX is outside the chip", (unsigned long)firstPage);
        return FLASH_STATUS_ERASE_ERROR;
    }

    FlashLock();
    memset(sFlashImage + firstPage * FLASH_BLOCK_SIZE, 0xFF, numPages * FLASH_BLOCK_SIZE);
    FlashMarkDirty();
    // The real driver reads the status, then clears it with osFlashClearStatus()
    status = sFlashStatus | GC_FLASH_STATUS_ERASE_DONE;
    sFlashStatus = 0;
    FlashUnlock();
    FlashNotifyWriter();

    return ((status & GC_FLASH_STATUS_ERASE_DONE) == GC_FLASH_STATUS_ERASE_DONE) ? FLASH_STATUS_ERASE_OK
                                                                                 : FLASH_STATUS_ERASE_ERROR;
}

s32 osFlashAllErase(void) {
    return FlashErase(0, GC_FLASH_NUM_PAGES);
}

s32 osFlashSectorErase(u32 pageNum) {
    return FlashErase(pageNum & ~(GC_FLASH_SECTOR_PAGES - 1), GC_FLASH_SECTOR_PAGES);
}

s32 osFlashWriteBuffer(OSIoMesg* mb, s32 priority, void* dramAddr, OSMesgQueue* mq) {
    FlashEnsureInit();

    mb->hdr.pri = priority;
    mb->hdr.retQueue = mq;
    mb->hdr.type = OS_MESG_TYPE_EDMAWRITE;
    mb->dramAddr = dramAddr;
    mb->devAddr = 0;
    mb->size = FLASH_BLOCK_SIZE;
    mb->piHandle = &sFlashHandle;

    FlashLock();
    memcpy(sFlashPageBuf, dramAddr, FLASH_BLOCK_SIZE);
    FlashUnlock();

    osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

s32 osFlashWriteArray(u32 pageNum) {
    u8* page;
    u8 status;
    s32 i;

    FlashEnsureInit();
    if (pageNum >= GC_FLASH_NUM_PAGES) {
        gc_log("flash: program of page 0x%lX is outside the chip", (unsigned long)pageNum);
        return FLASH_STATUS_WRITE_ERROR;
    }

    FlashLock();
    page = sFlashImage + pageNum * FLASH_BLOCK_SIZE;
    for (i = 0; i < FLASH_BLOCK_SIZE; i++) {
        page[i] &= sFlashPageBuf[i];
    }
    FlashMarkDirty();
    status = sFlashStatus | GC_FLASH_STATUS_WRITE_DONE;
    sFlashStatus = 0;
    FlashUnlock();
    FlashNotifyWriter();

    return ((status & GC_FLASH_STATUS_WRITE_DONE) == GC_FLASH_STATUS_WRITE_DONE) ? FLASH_STATUS_WRITE_OK
                                                                                 : FLASH_STATUS_WRITE_ERROR;
}

s32 osFlashReadArray(OSIoMesg* mb, s32 priority, u32 pageNum, void* dramAddr, u32 pageCount, OSMesgQueue* mq) {
    u32 size = pageCount * FLASH_BLOCK_SIZE;
    u32 avail = 0;
    u32 copy;

    FlashEnsureInit();

    mb->hdr.pri = priority;
    mb->hdr.retQueue = mq;
    mb->hdr.type = OS_MESG_TYPE_EDMAREAD;
    mb->dramAddr = dramAddr;
    mb->devAddr = pageNum * FLASH_BLOCK_SIZE;
    mb->size = size;
    mb->piHandle = &sFlashHandle;

    if (pageNum < GC_FLASH_NUM_PAGES) {
        avail = (GC_FLASH_NUM_PAGES - pageNum) * FLASH_BLOCK_SIZE;
    }
    copy = (size < avail) ? size : avail;

    if (copy != 0) {
        FlashLock();
        memcpy(dramAddr, sFlashImage + pageNum * FLASH_BLOCK_SIZE, copy);
        FlashUnlock();
    }
    if (copy < size) {
        gc_log("flash: read of pages 0x%lX+0x%lX goes past the end of the chip", (unsigned long)pageNum,
               (unsigned long)pageCount);
        memset((u8*)dramAddr + copy, 0xFF, size - copy);
    }

    osSendMesg(mq, (OSMesg)mb, OS_MESG_NOBLOCK);
    return 0;
}

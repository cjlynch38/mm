/**
 * libultra Peripheral Interface API (cartridge ROM) for the GameCube: osCartRomInit,
 * osCreatePiManager, osEPiStartDma.
 *
 * There is no PI manager thread. osEPiStartDma() copies the data synchronously in the calling
 * thread with gc_rom_read() (which serialises SD access and serves the resident audio range from
 * RAM), then sends the I/O message to its return queue NOBLOCK, as __osDevMgrMain does when the
 * DMA-done interrupt arrives. Callers always block on that queue afterwards, so the observable
 * order is the same; OS_MESG_PRI_HIGH (jam to the front of the PI command queue) has no meaning
 * when nothing is queued.
 */
#include "gc_ultra_internal.h"
#include "stdbool.h"

// Plausible cartridge timing, decoded from the first ROM word (0x80371240) as osCartRomInit does
#define GC_CART_LATENCY 0x40
#define GC_CART_PULSE 0x12
#define GC_CART_PAGE_SIZE 0x07
#define GC_CART_REL_DURATION 0x03

static OSPiHandle sCartRomHandle;
static s32 sCartRomNeedsInit = true;
static s32 sPiMgrActive;

OSPiHandle* osCartRomInit(void) {
    gc_os_lock();
    if (sCartRomNeedsInit) {
        sCartRomNeedsInit = false;
        sCartRomHandle.type = DEVICE_TYPE_CART;
        sCartRomHandle.baseAddress = PHYS_TO_K1(PI_DOM1_ADDR2);
        sCartRomHandle.domain = PI_DOMAIN1;
        sCartRomHandle.speed = 0;
        bzero(&sCartRomHandle.transferInfo, sizeof(__OSTranxInfo));
        sCartRomHandle.latency = GC_CART_LATENCY;
        sCartRomHandle.pageSize = GC_CART_PAGE_SIZE;
        sCartRomHandle.relDuration = GC_CART_REL_DURATION;
        sCartRomHandle.pulse = GC_CART_PULSE;
    }
    gc_os_unlock();

    return &sCartRomHandle;
}

void osCreatePiManager(OSPri pri, OSMesgQueue* cmdQ, OSMesg* cmdBuf, s32 cmdMsgCnt) {
    gc_os_lock();
    if (sPiMgrActive) {
        gc_os_unlock();
        return;
    }
    sPiMgrActive = true;
    gc_os_unlock();

    // Nothing is ever sent to the command queue, but create it like libultra so it reads as empty
    osCreateMesgQueue(cmdQ, cmdBuf, cmdMsgCnt);
}

/**
 * The PI DMAs to a physical address (osVirtualToPhysical), so KSEG0 and KSEG1 pointers reach the
 * same RAM. On GameCube only the cached mirror at 0x80000000 is usable: map KSEG1 aliases onto it.
 */
static void* PiDramPtr(void* dramAddr) {
    uintptr_t addr = (uintptr_t)dramAddr;

    if ((addr >= K1BASE) && (addr < K1BASE + 0x20000000)) {
        return (void*)(addr - K1BASE + K0BASE);
    }
    return dramAddr;
}

s32 osEPiStartDma(OSPiHandle* pihandle, OSIoMesg* mb, s32 direction) {
    uintptr_t cartAddr;

    if (!sPiMgrActive) {
        gc_log("pi: osEPiStartDma before osCreatePiManager");
        return -1;
    }

    mb->piHandle = pihandle;
    mb->hdr.type = (direction == OS_READ) ? OS_MESG_TYPE_EDMAREAD : OS_MESG_TYPE_EDMAWRITE;

    if ((pihandle == NULL) || (pihandle->type != DEVICE_TYPE_CART)) {
        gc_halt("pi: osEPiStartDma on a non-cartridge handle (type %d, devAddr 0x%08lX)",
                (pihandle != NULL) ? (int)pihandle->type : -1, (unsigned long)mb->devAddr);
    }
    if (direction != OS_READ) {
        gc_log("pi: osEPiStartDma(OS_WRITE) devAddr 0x%08lX size 0x%lX", (unsigned long)mb->devAddr,
               (unsigned long)mb->size);
        GC_TODO("osEPiStartDma(OS_WRITE)");
    }

    // Same address as __osEPiRawStartDma: K1_TO_PHYS(baseAddress | devAddr), relative to the
    // start of cartridge domain 1, i.e. the plain ROM offset for the cartridge handle.
    cartAddr = K1_TO_PHYS(pihandle->baseAddress | mb->devAddr);
    if (cartAddr < PI_DOM1_ADDR2) {
        gc_halt("pi: cartridge address 0x%08lX is outside the ROM", (unsigned long)cartAddr);
    }
    if (gc_rom_read(cartAddr - PI_DOM1_ADDR2, PiDramPtr(mb->dramAddr), mb->size) != 0) {
        gc_halt("pi: ROM read failed (offset 0x%08lX, size 0x%lX)", (unsigned long)(cartAddr - PI_DOM1_ADDR2),
                (unsigned long)mb->size);
    }

    if (mb->hdr.retQueue != NULL) {
        osSendMesg(mb->hdr.retQueue, (OSMesg)mb, OS_MESG_NOBLOCK);
    }
    return 0;
}

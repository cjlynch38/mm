/**
 * Runs MM's audio microcode on mupen64plus-rsp-cxd4's interpreter core (su.c and vu/, built from the
 * source tree the Makefile's CXD4 variable points to), as the reference for the audio host test.
 *
 * This replaces cxd4's plugin layer (module.c): it provides the RSP_INFO and the few symbols the core
 * expects, points the core at its own RDRAM/DMEM/IMEM, and starts the microcode at IMEM 0 like
 * osSpTaskLoad + osSpTaskStartGo do for the audio task (whose boot microcode is aspMain itself).
 *
 * cxd4 keeps RDRAM and SP memory as native 32-bit words: on a little-endian host, byte address a is at
 * a ^ 3. The images the test passes in are plain big-endian bytes, converted on the way in and out.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rsp.h"
#include "su.h"
#include "lle.h"

/* Symbols the interpreter core expects from cxd4's plugin layer */
RSP_INFO RCP_info_SP;
p_func GBI_phase;

void message(const char* body) {
    fprintf(stderr, "cxd4: %s\n", body);
}

extern ALIGNED i16 VR[32][N << VR_STATIC_WRAPAROUND];
extern ALIGNED i16 VACC[3][N];
extern ALIGNED i16 cf_ne[N];
extern ALIGNED i16 cf_co[N];
extern ALIGNED i16 cf_clip[N];
extern ALIGNED i16 cf_comp[N];
extern ALIGNED i16 cf_vce[N];

static uint8_t* sRdram;
static uint8_t sSpMem[0x2000]; // DMEM, then IMEM

static struct {
    u32 miIntr;
    u32 spMemAddr, spDramAddr, spRdLen, spWrLen, spStatus, spDmaFull, spDmaBusy, spPc, spSemaphore;
    u32 dpcStart, dpcEnd, dpcCurrent, dpcStatus, dpcClock, dpcBufBusy, dpcPipeBusy, dpcTmem;
} sRegs;

static void Lle_Nop(void) {
}

static void Lle_Timeout(int sig) {
    (void)sig;
    // aspMain keeps the current command in $26/$25 and the bytes of list left in $27
    fprintf(stderr, "lle: the microcode did not stop within 20 s (command %08X %08X, %d bytes left)\n",
            (unsigned)SR[26], (unsigned)SR[25], (int)SR[27]);
    _exit(3);
}

/** Copy a big-endian image to cxd4's word-swapped memory, or back (the swap is its own inverse) */
static void Lle_Swap(uint8_t* dst, const uint8_t* src, size_t size) {
    size_t i;
    uint32_t w;

    for (i = 0; i < size; i += 4) {
        memcpy(&w, src + i, 4);
        w = __builtin_bswap32(w);
        memcpy(dst + i, &w, 4);
    }
}

int Lle_Init(void) {
    sRdram = calloc(1, LLE_RAM_SIZE + 0x10000);
    if (sRdram == NULL) {
        return -1;
    }

    memset(&RCP_info_SP, 0, sizeof(RCP_info_SP));
    RCP_info_SP.RDRAM = sRdram;
    RCP_info_SP.DMEM = &sSpMem[0];
    RCP_info_SP.IMEM = &sSpMem[0x1000];
    RCP_info_SP.MI_INTR_REG = &sRegs.miIntr;
    RCP_info_SP.SP_MEM_ADDR_REG = &sRegs.spMemAddr;
    RCP_info_SP.SP_DRAM_ADDR_REG = &sRegs.spDramAddr;
    RCP_info_SP.SP_RD_LEN_REG = &sRegs.spRdLen;
    RCP_info_SP.SP_WR_LEN_REG = &sRegs.spWrLen;
    RCP_info_SP.SP_STATUS_REG = &sRegs.spStatus;
    RCP_info_SP.SP_DMA_FULL_REG = &sRegs.spDmaFull;
    RCP_info_SP.SP_DMA_BUSY_REG = &sRegs.spDmaBusy;
    RCP_info_SP.SP_PC_REG = &sRegs.spPc;
    RCP_info_SP.SP_SEMAPHORE_REG = &sRegs.spSemaphore;
    RCP_info_SP.DPC_START_REG = &sRegs.dpcStart;
    RCP_info_SP.DPC_END_REG = &sRegs.dpcEnd;
    RCP_info_SP.DPC_CURRENT_REG = &sRegs.dpcCurrent;
    RCP_info_SP.DPC_STATUS_REG = &sRegs.dpcStatus;
    RCP_info_SP.DPC_CLOCK_REG = &sRegs.dpcClock;
    RCP_info_SP.DPC_BUFBUSY_REG = &sRegs.dpcBufBusy;
    RCP_info_SP.DPC_PIPEBUSY_REG = &sRegs.dpcPipeBusy;
    RCP_info_SP.DPC_TMEM_REG = &sRegs.dpcTmem;
    RCP_info_SP.CheckInterrupts = Lle_Nop;
    GBI_phase = Lle_Nop;

    DRAM = sRdram;
    DMEM = &sSpMem[0];
    IMEM = &sSpMem[0x1000];
    CR[0x0] = &sRegs.spMemAddr;
    CR[0x1] = &sRegs.spDramAddr;
    CR[0x2] = &sRegs.spRdLen;
    CR[0x3] = &sRegs.spWrLen;
    CR[0x4] = &sRegs.spStatus;
    CR[0x5] = &sRegs.spDmaFull;
    CR[0x6] = &sRegs.spDmaBusy;
    CR[0x7] = &sRegs.spSemaphore;
    CR[0x8] = &sRegs.dpcStart;
    CR[0x9] = &sRegs.dpcEnd;
    CR[0xA] = &sRegs.dpcCurrent;
    CR[0xB] = &sRegs.dpcStatus;
    CR[0xC] = &sRegs.dpcClock;
    CR[0xD] = &sRegs.dpcBufBusy;
    CR[0xE] = &sRegs.dpcPipeBusy;
    CR[0xF] = &sRegs.dpcTmem;
    su_max_address = LLE_RAM_SIZE - 1;
    memset(conf, 0, 32); // no HLE: run every task on the interpreter
    MF_SP_STATUS_TIMEOUT = 32767;

    Lle_ResetRegs();
    signal(SIGALRM, Lle_Timeout);
    return 0;
}

void Lle_ResetRegs(void) {
    memset(VR, 0, sizeof(VR));
    memset(VACC, 0, sizeof(VACC));
    memset(cf_ne, 0, sizeof(cf_ne));
    memset(cf_co, 0, sizeof(cf_co));
    memset(cf_clip, 0, sizeof(cf_clip));
    memset(cf_comp, 0, sizeof(cf_comp));
    memset(cf_vce, 0, sizeof(cf_vce));
    memset(SR, 0, 32 * sizeof(SR[0]));
}

long Lle_Run(uint8_t* ram, uint8_t* dmem, const uint8_t* imem) {
    Lle_Swap(sRdram, ram, LLE_RAM_SIZE);
    Lle_Swap(&sSpMem[0], dmem, 0x1000);
    Lle_Swap(&sSpMem[0x1000], imem, 0x1000);

    sRegs.spStatus = 0; // running (osSpTaskStartGo clears HALT)
    sRegs.spPc = 0x04001000;
    sRegs.spSemaphore = 0;
    sRegs.spDmaBusy = 0;
    sRegs.spDmaFull = 0;
    sRegs.dpcStatus = 0;
    sRegs.miIntr = 0;

    alarm(20);
    run_task();
    alarm(0);

    Lle_Swap(ram, sRdram, LLE_RAM_SIZE);
    Lle_Swap(dmem, &sSpMem[0], 0x1000);
    return (sRegs.spStatus & 0x2) ? 0 : -1; // BROKE: the microcode ended with BREAK
}

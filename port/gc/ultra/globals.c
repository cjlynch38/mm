/**
 * libultra system globals and small OS functions with no GameCube equivalent: the IPL-provided
 * globals (N64 parameters.s), osGetMemSize, osInitialize, TLB and FPU control-register stubs.
 */
#include "gc_os_core.h"
#include "alignment.h"

// The game requires the 8 MB Expansion Pak (sys_initial_check.c) and an NTSC or MPAL console
#define GC_N64_MEM_SIZE 0x800000

s32 osTvType = OS_TV_NTSC;
s32 osResetType = 0; // cold reset: z_nmi_buff.c initialises its NMI buffer
u32 osMemSize = GC_N64_MEM_SIZE;
// Survives an N64 reset (NMI); on the GameCube it is simply zeroed at boot
s32 osAppNMIBuffer[OS_APP_NMI_BUFSIZE / sizeof(s32)] ALIGNED(8);

// The N64 FPU control/status register. Gekko has its own FPSCR; this value has no effect.
static u32 sFpcCsr = FPCSR_FS | FPCSR_EV;

u32 osGetMemSize(void) {
    return GC_N64_MEM_SIZE;
}

void __osInitialize_common(void) {
    gc_log("osInitialize: tv %d, memory 0x%X, reset type %d", (int)osTvType, (unsigned int)osMemSize,
           (int)osResetType);
}

void osUnmapTLBAll(void) {
}

// osAfterPreNMI() is in sp.c (it halts the emulated RSP)

u32 __osSetFpcCsr(u32 value) {
    u32 prev = sFpcCsr;

    sFpcCsr = value;
    return prev;
}

u32 __osGetFpcCsr(void) {
    return sFpcCsr;
}

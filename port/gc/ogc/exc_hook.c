/**
 * CPU exception log: when the game takes an exception nothing handles (DSI, ISI, alignment, program,
 * floating point unavailable, ...), write the exception, the registers, a stack backtrace and what the
 * game was doing to the log channel before libogc's own panic screen.
 *
 * libogc's own panic dump (registers and a raw stack dump) goes to the text console, which is not on
 * screen once the GX renderer owns the display, and to the USB Gecko only where the console is mirrored
 * there. This log adds what that dump lacks: the kind of exception, DAR/DSISR for data faults, the
 * return addresses of the backtrace, and the gamestate, frame, scene and room the game was in.
 * libogc installs its panic function (PPCExcptCurPanicFn = __libogc_panic) from SYS_Init, in the weak
 * __SYS_InstallPanicHandler; this file overrides that function to install Gc_ExcPanic, which chains
 * to __libogc_panic afterwards.
 *
 * The handler runs in exception context with interrupts off, on whatever stack the thread had. It
 * formats into a static buffer and writes to the USB Gecko directly (gc_log takes a mutex, which the
 * crashed thread may hold), or to SYS_Report when no Gecko answered at boot. Addresses resolve with
 * powerpc-eabi-addr2line -f -e build/gc-n64-us/mm-gc.elf <pc> <lr> <frames...>.
 */
#include <gccore.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <tuxedo/ppc/exception.h>
#include "gc_ogc.h"

#define EXC_GECKO_CHANNEL 1
#define EXC_GECKO_RETRIES 10000
#define EXC_LINE_MAX 256
#define EXC_BACKTRACE_MAX 24

void __libogc_panic(unsigned exid, PPCContext* ctx);
void __SYS_InstallPanicHandler(void);

static char sExcLine[EXC_LINE_MAX];
static volatile int sExcDepth;

static const char* exc_name(unsigned exid) {
    switch (exid) {
        case PPC_EXCPT_RESET:
            return "system reset";
        case PPC_EXCPT_MCHK:
            return "machine check";
        case PPC_EXCPT_DSI:
            return "DSI (data access)";
        case PPC_EXCPT_ISI:
            return "ISI (instruction fetch)";
        case PPC_EXCPT_ALIGN:
            return "alignment";
        case PPC_EXCPT_UNDEF:
            return "program (illegal instruction or trap)";
        case PPC_EXCPT_FPU:
            return "floating point unavailable";
        case PPC_EXCPT_DECR:
            return "decrementer";
        case PPC_EXCPT_SYSCALL:
            return "system call";
        case PPC_EXCPT_TRACE:
            return "trace";
        case PPC_EXCPT_PM:
            return "performance monitor";
        case PPC_EXCPT_BKPT:
            return "breakpoint";
        default:
            return "?";
    }
}

static void exc_emit(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void exc_emit(const char* fmt, ...) {
    va_list args;
    size_t len;

    va_start(args, fmt);
    vsnprintf(sExcLine, sizeof(sExcLine) - 1, fmt, args);
    va_end(args);
    len = strlen(sExcLine);
    sExcLine[len++] = '\n';
    sExcLine[len] = '\0';

    if (gc_ogc_log_gecko()) {
        usb_sendbuffer_safe_ex(EXC_GECKO_CHANNEL, sExcLine, len, EXC_GECKO_RETRIES);
    } else {
        SYS_Report("%s", sExcLine);
    }
}

/** Default for builds without the game (the test DOLs); port/gc/game/trace.c has the real one. */
__attribute__((weak)) void gc_game_crash_info(int* gameState, unsigned int* frames, int* scene, int* room,
                                              unsigned int* entrance) {
}

/** A stack address the backtrace may read: 4-byte aligned, cached MEM1. */
static int exc_stack_ok(u32 sp) {
    return ((sp & 3) == 0) && (sp >= 0x80000000) && (sp < 0x817FFFF8);
}

static void Gc_ExcPanic(unsigned exid, PPCContext* ctx) {
    int gameState = -1;
    unsigned int frames = 0;
    int scene = -1;
    int room = -1;
    unsigned int entrance = 0;
    u32 sp;
    int i;

    if (sExcDepth++ == 0) {
        exc_emit("EXCEPTION %u: %s in LWP %08X", exid, exc_name(exid), (unsigned int)LWP_GetSelf());
        exc_emit("  pc %08X lr %08X msr %08X cr %08X ctr %08X xer %08X", ctx->pc, ctx->lr, ctx->msr, ctx->cr,
                 ctx->ctr, ctx->xer);
        if ((exid == PPC_EXCPT_DSI) || (exid == PPC_EXCPT_ALIGN)) {
            u32 dar;
            u32 dsisr;

            __asm__ volatile("mfdar %0" : "=r"(dar));
            __asm__ volatile("mfdsisr %0" : "=r"(dsisr));
            exc_emit("  dar %08X dsisr %08X", dar, dsisr);
        }
        for (i = 0; i < 32; i += 4) {
            exc_emit("  r%-2d %08X %08X %08X %08X", i, ctx->gpr[i], ctx->gpr[i + 1], ctx->gpr[i + 2],
                     ctx->gpr[i + 3]);
        }

        // EABI frames: 0(sp) is the caller's frame, 4(that frame) the return address saved in it
        exc_emit("  backtrace (return addresses, innermost first):");
        sp = ctx->gpr[1];
        for (i = 0; (i < EXC_BACKTRACE_MAX) && exc_stack_ok(sp); i++) {
            u32 next = *(u32*)sp;

            if (!exc_stack_ok(next) || (next <= sp)) {
                break;
            }
            exc_emit("    %08X (frame %08X)", *(u32*)(next + 4), next);
            sp = next;
        }

        gc_game_crash_info(&gameState, &frames, &scene, &room, &entrance);
        if (scene >= 0) {
            exc_emit("  game: gamestate %d frame %u, scene %02X room %d, entrance %04X", gameState, frames,
                     (unsigned int)scene, room, entrance);
        } else {
            exc_emit("  game: gamestate %d frame %u", gameState, frames);
        }
    } else {
        exc_emit("EXCEPTION %u: %s inside the exception log (pc %08X)", exid, exc_name(exid), ctx->pc);
    }

    // libogc's dump is drawn into the text console's XFB, which is hidden while the renderer owns the
    // display: show it, so a crash on hardware without a USB Gecko is readable on the TV.
    gc_ogc_video_show_console();
    __libogc_panic(exid, ctx);
}

void __SYS_InstallPanicHandler(void) {
    PPCExcptCurPanicFn = Gc_ExcPanic;
}

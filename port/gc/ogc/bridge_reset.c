/**
 * Reset button.
 *
 * libogc calls the SYS_SetResetCallback handler in interrupt context when the button goes down,
 * and again for every further interrupt (it does not mask the line after a press, so contact
 * bounce can arrive as several presses). The handler only posts an LWP semaphore (safe from
 * interrupts). Two threads outside the game do the rest:
 *  - the press thread takes the presses. After each one it waits for the button to be released
 *    and drops the interrupts that arrived meanwhile, so one physical press is one event. The
 *    first press is handed to the callback thread; any later press, a press while no callback is
 *    registered yet, and any press after gc_halt restart the console (SYS_HOTRESET). It never
 *    takes a lock the game can hold and never logs, so Reset keeps working when the game is
 *    halted or deadlocked;
 *  - the callback thread calls the function the libultra side registered (it posts
 *    OS_EVENT_PRENMI, taking the OS lock) from normal thread context, once.
 * On N64 the console resets itself about half a second after PRENMI. Here the libultra side
 * decides what happens after PRENMI; the second press is the safety net.
 * Neither thread is created with gc_thread_create, so gc_halt does not stop them.
 */
#include <gccore.h>
#include <unistd.h>
#include "gc_ogc.h"

#define RESET_STACK_SIZE 0x4000

/* Quiet time after a release before the next press counts */
#define RESET_DEBOUNCE_MS 100

/* Above the callback thread, so a callback blocked on the game's locks never delays a restart */
#define GC_PRIO_SERVICE_RESET_PRESS (GC_PRIO_SERVICE_RESET + 1)

static sem_t sPressSem = LWP_SEM_NULL;    /* posted by the interrupt handler, once per interrupt */
static sem_t sCallbackSem = LWP_SEM_NULL; /* posted by the press thread for the first press */
static void (*volatile sResetCallback)(void);

static void reset_isr(u32 irq, void* ctx) {
    LWP_SemPost(sPressSem);
}

static void* reset_press_thread(void* arg) {
    int delivered = 0;

    for (;;) {
        u32 extra;

        LWP_SemWait(sPressSem);
        if (!delivered && !gc_ogc_halted() && sResetCallback != NULL) {
            delivered = 1;
            LWP_SemPost(sCallbackSem);
        } else {
            SYS_ResetSystem(SYS_HOTRESET, 0, false);
        }

        // One press can raise several interrupts (contact bounce, or the line firing again while
        // the button is held): wait for the release, then forget the interrupts seen meanwhile.
        while (SYS_ResetButtonDown()) {
            usleep(10000);
        }
        usleep(RESET_DEBOUNCE_MS * 1000);
        while (LWP_SemGetValue(sPressSem, &extra) == 0 && extra != 0) {
            LWP_SemWait(sPressSem);
        }
    }
    return NULL;
}

static void* reset_callback_thread(void* arg) {
    for (;;) {
        LWP_SemWait(sCallbackSem);
        gc_log("Reset: button pressed (press again to restart the console)");
        sResetCallback();
        // The log file gets the lines up to the reset (a second press restarts without writing it)
        gc_ogc_log_flush();
    }
    return NULL;
}

void gc_ogc_reset_init(void) {
    static lwp_t sPressThread = LWP_THREAD_NULL;
    lwp_t callbackThread;
    unsigned char* stacks;

    if (sPressThread != LWP_THREAD_NULL) {
        return;
    }
    stacks = gc_mem_alloc(2 * RESET_STACK_SIZE, 32);
    if (stacks == NULL || LWP_SemInit(&sPressSem, 0, 1) != 0 || LWP_SemInit(&sCallbackSem, 0, 1) != 0 ||
        LWP_CreateThread(&callbackThread, reset_callback_thread, NULL, stacks, RESET_STACK_SIZE,
                         GC_PRIO_SERVICE_RESET) != 0 ||
        LWP_CreateThread(&sPressThread, reset_press_thread, NULL, stacks + RESET_STACK_SIZE, RESET_STACK_SIZE,
                         GC_PRIO_SERVICE_RESET_PRESS) != 0) {
        sPressThread = LWP_THREAD_NULL;
        gc_log("Reset: cannot start the reset service; the Reset button is ignored");
        return;
    }
    SYS_SetResetCallback(reset_isr);
}

void gc_set_reset_callback(void (*callback)(void)) {
    sResetCallback = callback;
}

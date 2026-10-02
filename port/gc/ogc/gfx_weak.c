/*
 * Weak no-op renderer entry points. They keep a link without the port/gc/gfx objects working
 * (console-only builds, half-written renderer); the real definitions in gfx/gfx_task.c win.
 */
#include "gc_bridge.h"

__attribute__((weak)) void gc_gfx_init(void) {
}

__attribute__((weak)) int gc_gfx_enabled(void) {
    return 0;
}

__attribute__((weak)) void gc_gfx_run_task(unsigned int dlist) {
    (void)dlist;
}

__attribute__((weak)) void gc_gfx_present(const void* framebuffer) {
    (void)framebuffer;
}

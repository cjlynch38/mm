/**
 * Weak no-op versions of the software RSP tasks that osSpTaskStartGo() runs (sp.c), so the link
 * works whether or not their real implementations are in the build yet. The real definitions win:
 *   - Gc_AudRunTask: the audio command list interpreter (port/gc/audio). Without it the AI plays the
 *     game's audio buffers as they are, which is silence.
 *   - Gc_NJpegRunTask: the NJPEG decoder for z_jpeg.c (port/gc/game/njpeg_cpu.c). Without it the
 *     JPEG images z_jpeg.c decodes stay undecoded.
 */
#include "gc_ultra_internal.h"

__attribute__((weak)) void Gc_AudRunTask(OSTask* task) {
}

__attribute__((weak)) void Gc_NJpegRunTask(OSTask* task) {
}

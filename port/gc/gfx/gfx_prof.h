/**
 * Optional per-phase profile of the renderer's CPU time (gfx_rsp.c owns it, gfx_gx.c marks its phases).
 * Off unless built with -DGFX_PROF=1 in GFX_CFLAGS, for example
 *   make -f Makefile.gc BUILD_DIR=build/gc-prof GFX_CFLAGS='$(OGC_CFLAGS) -Iport/gc/gfx -Iport/gc/ogc
 *       -Ibuild/gc-prof/generated -DGFX_PROF=1'
 * Time is exclusive: a phase entered inside another one is not counted in the outer one. gfx_rsp_stats_frame logs
 * "gfx_prof:" lines every few seconds (microseconds and calls per task). The timer calls cost time themselves
 * (two timebase reads per phase change), so compare a profile build's totals with an ordinary build's.
 */
#ifndef GFX_PROF_H
#define GFX_PROF_H

#ifndef GFX_PROF
#define GFX_PROF 0
#endif

enum {
    GFX_PROF_OTHER, /* task setup outside the phases below (gfx_tex_frame, ...) */
    GFX_PROF_DL,    /* display list interpreter and the small RSP commands */
    GFX_PROF_VTX,   /* G_VTX: transform, clip codes, lighting, texgen, fog */
    GFX_PROF_TRI,   /* G_TRI*: rejection and culling */
    GFX_PROF_GXTRI, /* gfx_gx_triangle: render target, batch vertices (texture coordinates) */
    GFX_PROF_PREP,  /* gx_prepare_slow outside the TEV and texture binds */
    GFX_PROF_TEV,   /* gfx_tev_apply */
    GFX_PROF_BIND,  /* texture binds (gfx_tex_bind: cache lookups, conversions) */
    GFX_PROF_FLUSH, /* gfx_gx_flush: vertices written to the GX FIFO */
    GFX_PROF_RDP,   /* gfx_rdp_command: RDP state, TMEM loads */
    GFX_PROF_RECT,  /* texture and fill rectangles */
    GFX_PROF_FB,    /* gfx_fb.c from gfx_gx.c: frame readbacks, off-screen passes */
    GFX_PROF_END,   /* gfx_gx_task_end: depth write (gfx_fb_task_end), EFB copy */
    GFX_PROF_WAIT,  /* waiting for GX at the end of a task (or for the previous task's copy) */
    GFX_PROF_S2D,   /* S2DEX2 commands */
    GFX_PROF_ATTR,  /* vertex attributes (lighting, fog, texture coordinates), computed when a drawn triangle needs them */
    GFX_PROF_VSETUP, /* G_VTX setup: MV * P, light directions */
    GFX_PROF_COUNT
};

#if GFX_PROF && !defined(GFX_HOST_TEST)
void gfx_prof_enter(int phase);
void gfx_prof_leave(void);
void gfx_prof_task_begin(void);
void gfx_prof_task_end(void);
#define GFX_PROF_ENTER(p) gfx_prof_enter(p)
#define GFX_PROF_LEAVE() gfx_prof_leave()
#define GFX_PROF_TASK_BEGIN() gfx_prof_task_begin()
#define GFX_PROF_TASK_END() gfx_prof_task_end()
#else
#define GFX_PROF_ENTER(p) ((void)0)
#define GFX_PROF_LEAVE() ((void)0)
#define GFX_PROF_TASK_BEGIN() ((void)0)
#define GFX_PROF_TASK_END() ((void)0)
#endif

#endif

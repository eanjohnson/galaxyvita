#ifndef GALAXYVITA_GX_GXM_H
#define GALAXYVITA_GX_GXM_H

#include "gx_cp.h"
#include "gx_render.h"

/* The GX sink that draws through sceGxm.
 *
 * ModernGekko's command processor decodes the FIFO and hands over whole
 * vertices; this turns them into GXM draws. The transform is the one the host
 * software rasteriser already proves correct against real frames
 * (tests/host/gx_soft.c): object space through the XF position matrix, through
 * the projection, into GX clip space. From there the GPU does the divide, so
 * near-plane clipping and perspective-correct interpolation are the hardware's
 * rather than ours.
 *
 * What is NOT here yet: TEV, texturing, lighting and the per-draw blend/depth
 * state. Those are what Dolphin's generated shaders are for, and they need the
 * shader cache warmed by UID first. Geometry and the transform come first
 * because they are what a wrong answer is visible in. */

/* regs is the live register file; it must outlive the sink. Returns 0 and
 * leaves gx_gxm_error() set on failure. */
int gx_gxm_init(const GxRegs *regs);
void gx_gxm_shutdown(void);

/* Fills in the sink to hand to gx_cp_set_sink(). */
void gx_gxm_sink(GxSink *out);

const char *gx_gxm_error(void);

/* One line for the boot log: frames presented, draws, vertices, and what was
 * dropped and why. */
void gx_gxm_report(char *out, unsigned n);

#endif

#ifndef GALAXYVITA_GX_SOFT_H
#define GALAXYVITA_GX_SOFT_H

#include "gx_cp.h"
#include "gx_render.h"

/* Software rasteriser for the decoded GX stream. It exists so the renderer can
 * be built and checked on this machine -- a host run is minutes, a Vita deploy
 * cycle is not -- and so the vitaGL backend is a port of something already
 * known to be correct rather than a debugging exercise on hardware. */
typedef struct SoftGfx SoftGfx;

SoftGfx *soft_gfx_create(const GxRegs *regs, const char *out_dir, int max_frames);
void soft_gfx_sink(SoftGfx *g, GxSink *out);
void soft_gfx_report(const SoftGfx *g);
void soft_gfx_destroy(SoftGfx *g);

#endif

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
 * Map 0 texturing, the per-draw blend and depth state (BP 0x40 and 0x41) and
 * the alpha test (BP 0xF3) are here; the texture is modulated by the raster
 * colour, which is what tests/host/gx_soft.c does in place of TEV so the two
 * renderers still diff frame for frame.
 *
 * What is NOT here yet: TEV proper, the maps past the first, lighting, and the
 * scissor rectangle. Those are what Dolphin's generated shaders are for, and
 * they need the shader cache warmed by UID first. */

/* regs is the live register file; it must outlive the sink. Returns 0 and
 * leaves gx_gxm_error() set on failure. */
int gx_gxm_init(const GxRegs *regs);
void gx_gxm_shutdown(void);

/* Fills in the sink to hand to gx_cp_set_sink(). */
void gx_gxm_sink(GxSink *out);

/* Writes the first `count` presented frames, one in every `stride`, to
 * <dir>/frame_NNNN.raw as tight 960x544 RGBA.
 *
 * This is the only way to check what the console actually drew: there is no
 * frame capture on the device and no way to read the screen except through the
 * display buffer the GPU just wrote. The host rasteriser dumps the same frames
 * as PNGs, so the two can be put side by side. Off unless this is called. */
void gx_gxm_dump_frames(const char *dir, unsigned count, unsigned stride);

/* Which fragment shader the textured path uses: 0 the real one (texture times
 * raster colour), 1 the texcoord varying as red/green, 2 the texture alone.
 * 1 and 2 exist to tell apart the causes of a white frame without a rebuild
 * per hypothesis. Must be called before gx_gxm_init(). */
void gx_gxm_set_frag_mode(unsigned mode);

/* How a decoded image becomes an SceGxmTexture: 0 linear with the ABGR
 * swizzle, 1 linear with RGBA, 2 linear with an explicit byte stride. The
 * console samples a verifiably correct texture as white and every stage
 * around this call has been measured, so the remaining candidates all live
 * in it; selecting them lets one device run try each. */
void gx_gxm_set_tex_init_mode(unsigned mode);

/* Drop the texture cache so the next bind re-creates under a new init mode.
 * Wait for the GPU first. For the selftest; the game never needs it. */
void gx_gxm_flush_textures(void);

/* Ask for the next presented frame to be written to <dump dir>/snap.raw. Any
 * serial different from the last one asks for one frame; repeating a serial
 * asks for nothing. Safe to call from another thread. */
void gx_gxm_snap(unsigned serial);

const char *gx_gxm_error(void);

/* One line for the boot log: frames presented, draws, vertices, and what was
 * dropped and why. */
void gx_gxm_report(char *out, unsigned n);

#endif

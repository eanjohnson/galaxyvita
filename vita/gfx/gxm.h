#ifndef GALAXYVITA_GXM_H
#define GALAXYVITA_GXM_H

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

/* The raw sceGxm device.
 *
 * Not vitaGL. The generated shaders reach the GPU through the console's own Cg
 * compiler, and vitaGL's runtime GLSL path (vitaShARK) returns "fatal internal
 * error" for every program including a two-line control shader -- so the GL
 * layer cannot carry what this backend needs. Everything here is the standard
 * GXM bring-up: context, render target, a double-buffered display queue, a
 * depth/stencil surface and a shader patcher. */

typedef struct GxmFrameStats {
  unsigned scenes;
  unsigned draws;
  unsigned vertices;
  unsigned dropped_full; /* geometry that did not fit the frame's arenas */
} GxmFrameStats;

int gxm_init(void);
void gxm_shutdown(void);

/* Non-NULL only between init and shutdown. */
SceGxmContext *gxm_context(void);
SceGxmShaderPatcher *gxm_patcher(void);

/* GPU-visible memory. `uid` receives the memblock so it can be freed; the
 * returned pointer is already mapped for the GPU with `attribs`. */
void *gxm_alloc(SceKernelMemBlockType type, unsigned int size,
                unsigned int alignment, unsigned int attribs, SceUID *uid);
void gxm_free(SceUID uid);

/* A scene per presented frame.
 *
 * There is no clear entry point here on purpose: GXM has no clear call, and
 * the way you clear is to draw a full-screen triangle -- which needs a program
 * and a vertex stream, both of which belong to the drawing layer, not to
 * device bring-up. gx_gxm.c does it with the same pipeline it draws with. */
void gxm_begin_scene(void);
void gxm_end_scene(void);

/* Blocks until every queued swap has retired. */
void gxm_finish(void);

/* Copies the buffer most recently handed to the display queue into `rgba` as
 * tight width*height*4 bytes. The display buffers live in CDRAM, which the CPU
 * can read, so a frame can be pulled off the console and diffed against the
 * host rasteriser's PNG of the same frame -- which is the only way to check
 * pixels without a camera pointed at the screen. */
int gxm_capture(unsigned char *rgba);

unsigned gxm_width(void);
unsigned gxm_height(void);
const char *gxm_error(void);

#endif

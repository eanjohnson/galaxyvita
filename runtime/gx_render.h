#ifndef GALAXYVITA_GX_RENDER_H
#define GALAXYVITA_GX_RENDER_H

#include "common/types.h"

/* One decoded vertex, in object space. The parser resolves every attribute --
 * direct or indexed through the CP array registers -- before handing it over,
 * so a backend never has to know how the vertex was encoded. */
typedef struct GxVertex {
  float pos[3];
  float nrm[3];
  float tex0[2];
  u8 color0[4];
  u8 pnmtx;    /* position/normal matrix index, in XF matrix rows */
  u8 tex0mtx;
  u8 has_color0;
  u8 has_tex0;
} GxVertex;

/* Where decoded geometry goes. Vertices are streamed rather than buffered: a
 * single primitive in this game reaches 65535 of them, and both backends
 * (software rasteriser here, vitaGL on the Vita) would rather assemble
 * triangles as they arrive than hold the whole strip. */
typedef struct GxSink {
  void *user;
  /* prim is the raw GX opcode: 0x80 quads, 0x90 triangles, 0x98 strip,
   * 0xA0 fan, 0xA8 lines, 0xB0 line strip, 0xB8 points. */
  void (*prim_begin)(void *user, u8 prim, u32 count);
  void (*vertex)(void *user, const GxVertex *v);
  void (*prim_end)(void *user);
  /* BP register 0x52, copy execute: the frame the game just finished. The
   * clear flag says whether the EFB is cleared as part of the copy. */
  void (*copy)(void *user, u32 data);
} GxSink;

#endif

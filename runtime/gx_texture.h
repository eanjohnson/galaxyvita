#ifndef GALAXYVITA_GX_TEXTURE_H
#define GALAXYVITA_GX_TEXTURE_H

#include "common/types.h"
#include "gx_cp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GX texture decode: tiled guest formats -> tight RGBA8.
 *
 * This was the host software rasteriser's, and it moved here the day the Vita
 * needed it too. Decoding is the half of texturing that has nothing to do with
 * the renderer -- the host turns the result into pixels itself and the console
 * hands it to sceGxm as a linear surface -- so sharing it means one decoder
 * proven by the host frame tests and running unchanged on the device, rather
 * than a second copy that drifts.
 *
 * Paletted formats (C4/C8/C14X2) are deliberately absent: they need TMEM TLUT
 * tracking, which ModernGekko does not model, and this game's boot binds none
 * (`gfx tex unsupported=0`). tile_geom() rejects them, so they arrive as a
 * counted miss rather than as garbage. */

enum {
  GX_TF_I4 = 0x0,
  GX_TF_I8 = 0x1,
  GX_TF_IA4 = 0x2,
  GX_TF_IA8 = 0x3,
  GX_TF_RGB565 = 0x4,
  GX_TF_RGB5A3 = 0x5,
  GX_TF_RGBA8 = 0x6,
  GX_TF_CMPR = 0xE
};

/* What a texmap is currently bound to, read out of the BP register file. */
typedef struct {
  u32 addr; /* physical guest address of the tiled image */
  u32 fmt;
  int w, h;
} GxTexRef;

/* Block width, block height and bytes per block for `fmt`. 0 for a format
 * this decoder does not handle. */
int gx_tex_tile_geom(u32 fmt, int *bw, int *bh, int *bytes);

/* Bytes the tiled image occupies, or 0 for an unhandled format. */
u32 gx_tex_size(u32 fmt, int w, int h);

/* Fill `out` from the BP registers for texmap `map` (0..7). Returns 0 when no
 * texture is bound there or the dimensions are implausible. */
int gx_tex_describe(const GxRegs *f, unsigned map, GxTexRef *out);

/* Is texturing on at all? BP 0x00 (GenMode) low nibble is the texture count,
 * and zero means the raster colour goes straight out. */
int gx_tex_enabled(const GxRegs *f);

/* Wrap modes for texmap 0, as the two-bit GX codes: 0 clamp, 1 repeat,
 * 2 mirror. */
void gx_tex_wrap(const GxRegs *f, u32 *wrap_s, u32 *wrap_t);

/* Decode `len` bytes of tiled source into `dst`, which must hold w*h*4.
 * Returns 0 for an unhandled format or a source too short to be the image. */
int gx_tex_decode(const u8 *src, u32 len, u32 fmt, int w, int h, u8 *dst);

#ifdef __cplusplus
}
#endif

#endif

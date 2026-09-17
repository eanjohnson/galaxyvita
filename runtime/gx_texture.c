#include "gx_texture.h"

#include <string.h>

/* Extracted from tests/host/gx_soft.c; see gx_texture.h for why it lives here
 * now. The decode itself is unchanged -- it is the version the host frame
 * tests already agree with, and a rewrite on the way to the console would have
 * put the one thing that was working back in doubt. */

static void rgb565_to_rgba(u16 v, u8 *out) {
  u32 r = (v >> 11) & 0x1Fu, g5 = (v >> 5) & 0x3Fu, b = v & 0x1Fu;
  out[0] = (u8)((r << 3) | (r >> 2));
  out[1] = (u8)((g5 << 2) | (g5 >> 4));
  out[2] = (u8)((b << 3) | (b >> 2));
  out[3] = 0xFF;
}

static void rgb5a3_to_rgba(u16 v, u8 *out) {
  if (v & 0x8000u) { /* 0RRRRRGGGGGBBBBB, opaque */
    u32 r = (v >> 10) & 0x1Fu, g = (v >> 5) & 0x1Fu, b = v & 0x1Fu;
    out[0] = (u8)((r << 3) | (r >> 2));
    out[1] = (u8)((g << 3) | (g >> 2));
    out[2] = (u8)((b << 3) | (b >> 2));
    out[3] = 0xFF;
  } else { /* AAARRRRGGGGBBBB */
    u32 a = (v >> 12) & 0x7u, r = (v >> 8) & 0xFu, g = (v >> 4) & 0xFu,
        b = v & 0xFu;
    out[0] = (u8)((r << 4) | r);
    out[1] = (u8)((g << 4) | g);
    out[2] = (u8)((b << 4) | b);
    out[3] = (u8)((a << 5) | (a << 2) | (a >> 1));
  }
}

static u16 be16(const u8 *p) { return (u16)(((u32)p[0] << 8) | p[1]); }

/* One 4x4 DXT1 block, GC flavour: big-endian endpoints and the 3-colour mode
 * when c0 <= c1 carries a transparent index. */
static void decode_cmpr_block(const u8 *src, u8 *dst, int pitch) {
  u16 c0 = be16(src), c1 = be16(src + 2);
  u8 pal[4][4];
  rgb565_to_rgba(c0, pal[0]);
  rgb565_to_rgba(c1, pal[1]);
  if (c0 > c1) {
    for (int k = 0; k < 3; k++) {
      pal[2][k] = (u8)((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = (u8)((pal[0][k] + 2 * pal[1][k]) / 3);
    }
    pal[2][3] = pal[3][3] = 0xFF;
  } else {
    for (int k = 0; k < 3; k++) {
      pal[2][k] = (u8)((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
    pal[2][3] = 0xFF;
    pal[3][3] = 0x00;
  }
  for (int y = 0; y < 4; y++) {
    u8 bits = src[4 + y];
    for (int x = 0; x < 4; x++) {
      u32 idx = (bits >> (6 - 2 * x)) & 3u;
      memcpy(dst + (size_t)y * (size_t)pitch + (size_t)x * 4u, pal[idx], 4);
    }
  }
}

/* Tile geometry per format: block width, block height, bytes per block. */
int gx_tex_tile_geom(u32 fmt, int *bw, int *bh, int *bytes) {
  switch (fmt) {
  case GX_TF_I4:
    *bw = 8; *bh = 8; *bytes = 32; return 1;
  case GX_TF_I8:
  case GX_TF_IA4:
    *bw = 8; *bh = 4; *bytes = 32; return 1;
  case GX_TF_IA8:
  case GX_TF_RGB565:
  case GX_TF_RGB5A3:
    *bw = 4; *bh = 4; *bytes = 32; return 1;
  case GX_TF_RGBA8:
    *bw = 4; *bh = 4; *bytes = 64; return 1;
  case GX_TF_CMPR:
    *bw = 8; *bh = 8; *bytes = 32; return 1;
  default:
    return 0;
  }
}

/* Decode a whole texture to RGBA8. Returns 0 for a format we do not handle. */
int gx_tex_decode(const u8 *src, u32 len, u32 fmt, int w, int h,
                  u8 *dst) {
  int bw, bh, bytes;
  if (!gx_tex_tile_geom(fmt, &bw, &bh, &bytes))
    return 0;
  int tw = (w + bw - 1) / bw, th = (h + bh - 1) / bh;
  if ((u32)tw * (u32)th * (u32)bytes > len)
    return 0;
  size_t pitch = (size_t)w * 4u;
  const u8 *p = src;
  for (int ty = 0; ty < th; ty++) {
    for (int tx = 0; tx < tw; tx++, p += bytes) {
      for (int y = 0; y < bh; y++) {
        int py = ty * bh + y;
        if (py >= h)
          continue;
        for (int x = 0; x < bw; x++) {
          int px = tx * bw + x;
          if (px >= w && fmt != GX_TF_CMPR)
            continue;
          u8 *o = dst + (size_t)py * pitch + (size_t)px * 4u;
          switch (fmt) {
          case GX_TF_I4: {
            u8 b = p[y * 4 + x / 2];
            u8 i = (u8)((x & 1) ? (b & 0x0Fu) : (b >> 4));
            i = (u8)((i << 4) | i);
            o[0] = o[1] = o[2] = o[3] = i;
            break;
          }
          case GX_TF_I8: {
            u8 i = p[y * 8 + x];
            o[0] = o[1] = o[2] = o[3] = i;
            break;
          }
          case GX_TF_IA4: {
            u8 b = p[y * 8 + x];
            u8 a = (u8)(b >> 4), i = (u8)(b & 0x0Fu);
            o[0] = o[1] = o[2] = (u8)((i << 4) | i);
            o[3] = (u8)((a << 4) | a);
            break;
          }
          case GX_TF_IA8: {
            const u8 *q = p + (y * 4 + x) * 2;
            o[0] = o[1] = o[2] = q[1];
            o[3] = q[0];
            break;
          }
          case GX_TF_RGB565:
            rgb565_to_rgba(be16(p + (y * 4 + x) * 2), o);
            break;
          case GX_TF_RGB5A3:
            rgb5a3_to_rgba(be16(p + (y * 4 + x) * 2), o);
            break;
          case GX_TF_RGBA8: {
            /* Two halves: 4x4 of AR then 4x4 of GB. */
            const u8 *ar = p + (y * 4 + x) * 2;
            const u8 *gb = p + 32 + (y * 4 + x) * 2;
            o[3] = ar[0];
            o[0] = ar[1];
            o[1] = gb[0];
            o[2] = gb[1];
            break;
          }
          default:
            break;
          }
        }
      }
      if (fmt == GX_TF_CMPR) {
        /* An 8x8 tile is four 4x4 DXT1 blocks in raster order. */
        for (int sb = 0; sb < 4; sb++) {
          int ox = tx * 8 + (sb & 1) * 4;
          int oy = ty * 8 + (sb >> 1) * 4;
          u8 tmp[4 * 4 * 4];
          decode_cmpr_block(p + sb * 8, tmp, 16);
          for (int y = 0; y < 4; y++) {
            if (oy + y >= h)
              continue;
            for (int x = 0; x < 4; x++) {
              if (ox + x >= w)
                continue;
              memcpy(dst + (size_t)(oy + y) * pitch + (size_t)(ox + x) * 4u,
                     tmp + (y * 4 + x) * 4, 4);
            }
          }
        }
      }
    }
  }
  return 1;
}

u32 gx_tex_size(u32 fmt, int w, int h) {
  int bw, bh, bytes;
  if (w <= 0 || h <= 0 || !gx_tex_tile_geom(fmt, &bw, &bh, &bytes))
    return 0u;
  return (u32)((w + bw - 1) / bw) * (u32)((h + bh - 1) / bh) * (u32)bytes;
}

int gx_tex_describe(const GxRegs *f, unsigned map, GxTexRef *out) {
  /* Maps 0-3 use BP 0x88/0x94, maps 4-7 the 0xA8/0xB4 mirror. */
  u32 img0, img3;
  if (map > 7u)
    return 0;
  img0 = (map < 4u) ? f->bp[0x88u + map] : f->bp[0xA8u + (map - 4u)];
  img3 = (map < 4u) ? f->bp[0x94u + map] : f->bp[0xB4u + (map - 4u)];
  if (!img0 && !img3)
    return 0;
  out->w = (int)(img0 & 0x3FFu) + 1;
  out->h = (int)((img0 >> 10) & 0x3FFu) + 1;
  out->fmt = (img0 >> 20) & 0xFu;
  out->addr = (img3 & 0x00FFFFFFu) << 5;
  if (out->w <= 0 || out->h <= 0 || out->w > 1024 || out->h > 1024)
    return 0;
  return 1;
}

int gx_tex_enabled(const GxRegs *f) { return (f->bp[0x00] & 0xFu) != 0u; }

void gx_tex_wrap(const GxRegs *f, u32 *wrap_s, u32 *wrap_t) {
  const u32 mode0 = f->bp[0x80u] & 0x00FFFFFFu;
  *wrap_s = mode0 & 3u;
  *wrap_t = (mode0 >> 2) & 3u;
}

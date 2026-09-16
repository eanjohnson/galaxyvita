#include "gx_soft.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The EFB is at most 640x528 on this hardware. Copies name a smaller source
 * rectangle; the buffer is allocated once at the maximum. */
enum { EFB_W = 640, EFB_H = 528 };

/* XF register file addresses we care about for transform. */
enum {
  XF_VIEWPORT = 0x101Au, /* 6 words: wd/2, -ht/2, zrange, xorig, yorig, farz */
  XF_PROJECTION = 0x1020u /* 6 params + type word */
};

enum { TEX_CACHE = 512 };

typedef struct {
  u32 addr, fmt;
  int w, h;
  u8 *px; /* decoded RGBA8, NULL if the format or address was unusable */
} Tex;

typedef struct {
  float x, y, z, w, iw;
  float u, v;
  u8 c[4];
  int ok;
} SVert;

struct SoftGfx {
  const GxRegs *fifo;
  u8 *color;
  float *depth;
  int w, h;

  u8 prim;
  SVert fan0, prev1, prev2;
  u32 in_prim;
  int parity;

  u32 tris_in;
  u32 tris_drawn;
  u32 verts;
  u32 no_matrix;
  u32 degenerate;
  u32 v_behind;   /* clip w <= 0: behind the eye */
  u32 v_novp;     /* viewport never loaded */
  u32 prim_index;
  int shade_prim;
  u32 stride;
  int trace;
  u32 scissored;
  u32 clipped_out;
  u32 drawn_persp, degen_persp, offscreen, offscreen_persp;
  u32 p_nocolor, p_scissor;
  u32 off_pc;
  float off_x0, off_x1, off_y0, off_y1;
  u64 p_zfail;
  u64 pixels_persp;
  u32 ttrace;
  u64 pixels;
  u32 persp_verts, ortho_verts, vtrace;
  Tex tex[TEX_CACHE];
  u32 tex_n, tex_decoded, tex_unmapped, tex_unsupported;
  u64 tex_pixels;
  u32 alpha_killed;
  int no_tex;
  u32 frames;
  u32 copies;
  u32 copies_xfb;
  int max_frames;
  char dir[240];
  /* Draw-state capture for the Dolphin shader probe: the register files a
   * shader UID is built from, one file per distinct TEV/texture configuration.
   * Off unless GV_GFX_STATEDUMP names a directory. */
  char st_dir[240];
  u64 st_seen[32];
  int st_n;
  int st_written;
};

/* ---------------------------------------------------------------------- */
/* PNG output. Stored (uncompressed) deflate blocks keep this dependency-free;
 * these are debug frames, not something that needs to be small. */

static u32 crc_table[256];
static int crc_ready;

static void crc_init(void) {
  for (u32 n = 0; n < 256; n++) {
    u32 c = n;
    for (int k = 0; k < 8; k++)
      c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
  crc_ready = 1;
}

static u32 crc32_buf(u32 crc, const u8 *p, size_t n) {
  if (!crc_ready)
    crc_init();
  crc ^= 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++)
    crc = crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

static void be32_put(u8 *p, u32 v) {
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}

static void png_chunk(FILE *f, const char *tag, const u8 *data, u32 len) {
  u8 hdr[8];
  be32_put(hdr, len);
  memcpy(hdr + 4, tag, 4);
  fwrite(hdr, 1, 8, f);
  if (len)
    fwrite(data, 1, len, f);
  u32 crc = crc32_buf(0, hdr + 4, 4);
  if (len)
    crc = crc32_buf(crc, data, len);
  u8 c[4];
  be32_put(c, crc);
  fwrite(c, 1, 4, f);
}

static void png_write(const char *path, const u8 *rgba, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f)
    return;
  static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);

  u8 ihdr[13];
  be32_put(ihdr, (u32)w);
  be32_put(ihdr + 4, (u32)h);
  ihdr[8] = 8;  /* bit depth */
  ihdr[9] = 6;  /* RGBA */
  ihdr[10] = 0;
  ihdr[11] = 0;
  ihdr[12] = 0;
  png_chunk(f, "IHDR", ihdr, sizeof(ihdr));

  size_t raw_len = (size_t)h * (1u + (size_t)w * 4u);
  u8 *raw = (u8 *)malloc(raw_len);
  if (!raw) {
    fclose(f);
    return;
  }
  for (int y = 0; y < h; y++) {
    u8 *row = raw + (size_t)y * (1u + (size_t)w * 4u);
    row[0] = 0; /* filter: none */
    memcpy(row + 1, rgba + (size_t)y * (size_t)w * 4u, (size_t)w * 4u);
  }

  /* zlib: 0x78 0x01, stored blocks, adler32. */
  size_t cap = 2 + raw_len + (raw_len / 65535 + 1) * 5 + 4;
  u8 *z = (u8 *)malloc(cap);
  if (!z) {
    free(raw);
    fclose(f);
    return;
  }
  size_t zi = 0;
  z[zi++] = 0x78;
  z[zi++] = 0x01;
  size_t off = 0;
  while (off < raw_len) {
    size_t n = raw_len - off;
    if (n > 65535)
      n = 65535;
    z[zi++] = (off + n >= raw_len) ? 1 : 0;
    z[zi++] = (u8)(n & 0xFF);
    z[zi++] = (u8)(n >> 8);
    z[zi++] = (u8)(~n & 0xFF);
    z[zi++] = (u8)((~n >> 8) & 0xFF);
    memcpy(z + zi, raw + off, n);
    zi += n;
    off += n;
  }
  u32 a = 1, b = 0;
  for (size_t i = 0; i < raw_len; i++) {
    a = (a + raw[i]) % 65521u;
    b = (b + a) % 65521u;
  }
  be32_put(z + zi, (b << 16) | a);
  zi += 4;
  png_chunk(f, "IDAT", z, (u32)zi);
  png_chunk(f, "IEND", NULL, 0);
  fclose(f);
  free(z);
  free(raw);
}

/* ---------------------------------------------------------------------- */
/* Transform.                                                              */

static float xf_f(const GxRegs *f, u32 addr) {
  union {
    u32 u;
    float f;
  } bits;
  bits.u = addr < GX_XF_WORDS ? f->xf[addr] : 0u;
  return bits.f;
}

/* Object -> clip -> screen. The position matrix is three rows of four floats
 * in XF matrix memory; PNMTXIDX counts rows, so the matrix starts at idx*4. */
static SVert transform(SoftGfx *g, const GxVertex *v) {
  SVert out;
  const GxRegs *f = g->fifo;
  memset(&out, 0, sizeof(out));
  memcpy(out.c, v->color0, 4);
  if (g->shade_prim) {
    /* Without textures or TEV every surface is white and the frame reads as a
     * blank page. Hashing the primitive index into a colour makes the layout
     * legible, which is what M1 needs to show. */
    u32 hsh = g->prim_index * 2654435761u;
    out.c[0] = (u8)(64u + ((hsh >> 0) & 0x7Fu));
    out.c[1] = (u8)(64u + ((hsh >> 9) & 0x7Fu));
    out.c[2] = (u8)(64u + ((hsh >> 18) & 0x7Fu));
    out.c[3] = 0xFF;
  }
  out.u = v->tex0[0];
  out.v = v->tex0[1];

  u32 base = (u32)v->pnmtx * 4u;
  if (base + 12u > 256u) {
    g->no_matrix++;
    return out;
  }
  float ex = 0, ey = 0, ez = 0;
  for (int r = 0; r < 3; r++) {
    float m0 = xf_f(f, base + (u32)r * 4u + 0);
    float m1 = xf_f(f, base + (u32)r * 4u + 1);
    float m2 = xf_f(f, base + (u32)r * 4u + 2);
    float m3 = xf_f(f, base + (u32)r * 4u + 3);
    float d = m0 * v->pos[0] + m1 * v->pos[1] + m2 * v->pos[2] + m3;
    if (r == 0)
      ex = d;
    else if (r == 1)
      ey = d;
    else
      ez = d;
  }

  float p0 = xf_f(f, XF_PROJECTION + 0), p1 = xf_f(f, XF_PROJECTION + 1);
  float p2 = xf_f(f, XF_PROJECTION + 2), p3 = xf_f(f, XF_PROJECTION + 3);
  float p4 = xf_f(f, XF_PROJECTION + 4), p5 = xf_f(f, XF_PROJECTION + 5);
  u32 ortho = f->xf[XF_PROJECTION + 6] & 1u;
  float cx, cy, cz, cw;
  if (ortho)
    g->ortho_verts++;
  else
    g->persp_verts++;
  if (ortho) {
    cx = p0 * ex + p1;
    cy = p2 * ey + p3;
    cz = p4 * ez + p5;
    cw = 1.0f;
  } else {
    cx = p0 * ex + p1 * ez;
    cy = p2 * ey + p3 * ez;
    cz = p4 * ez + p5;
    cw = -ez;
  }
  if (!(cw > 1e-6f))
    g->v_behind++; /* counted, not dropped -- the clipper handles it */

  float vw = xf_f(f, XF_VIEWPORT + 0);
  if (vw == 0.0f) {
    g->v_novp++;
    return out;
  }
  out.x = cx;
  out.y = cy;
  out.z = cz;
  out.w = cw;
  out.ok = 1;
  return out;
}

/* Clip space -> screen. The 342 bias is the fixed offset GX bakes into the
 * viewport origin. */
static void project(SoftGfx *g, SVert *s) {
  const GxRegs *f = g->fifo;
  float inv = 1.0f / s->w;
  float nx = s->x * inv, ny = s->y * inv, nz = s->z * inv;
  float vw = xf_f(f, XF_VIEWPORT + 0), vh = xf_f(f, XF_VIEWPORT + 1);
  float vz = xf_f(f, XF_VIEWPORT + 2), vx = xf_f(f, XF_VIEWPORT + 3);
  float vy = xf_f(f, XF_VIEWPORT + 4), vf = xf_f(f, XF_VIEWPORT + 5);
  s->x = nx * vw + (vx - 342.0f);
  s->y = ny * vh + (vy - 342.0f);
  s->z = nz * vz + vf;
  s->iw = inv;
}

static SVert lerp_vert(const SVert *a, const SVert *b, float t) {
  SVert o;
  o.x = a->x + (b->x - a->x) * t;
  o.y = a->y + (b->y - a->y) * t;
  o.z = a->z + (b->z - a->z) * t;
  o.w = a->w + (b->w - a->w) * t;
  o.iw = 0.0f;
  o.u = a->u + (b->u - a->u) * t;
  o.v = a->v + (b->v - a->v) * t;
  for (int k = 0; k < 4; k++)
    o.c[k] = (u8)((float)a->c[k] + ((float)b->c[k] - (float)a->c[k]) * t);
  o.ok = 1;
  return o;
}

/* ---------------------------------------------------------------------- */
/* Texture unit.                                                           */
/*                                                                         */
/* GC textures live in main RAM, tiled, in one of a handful of formats. The */
/* BP registers name the address, size and format; the pixels themselves    */
/* are fetched through the same guest-memory hook the FIFO uses for display */
/* lists and indexed arrays. Decoded images are cached by (address, format, */
/* size) -- the title screen re-binds the same handful every frame.         */

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
static int tile_geom(u32 fmt, int *bw, int *bh, int *bytes) {
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
static int decode_texture(const u8 *src, u32 len, u32 fmt, int w, int h,
                          u8 *dst) {
  int bw, bh, bytes;
  if (!tile_geom(fmt, &bw, &bh, &bytes))
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

/* Bind texmap `map` (0..7) and return its decoded image, or NULL. */
static const Tex *tex_bind(SoftGfx *g, unsigned map) {
  const GxRegs *f = g->fifo;
  /* Maps 0-3 use BP 0x88/0x94, maps 4-7 the 0xA8/0xB4 mirror. */
  u32 img0 = (map < 4u) ? f->bp[0x88u + map] : f->bp[0xA8u + (map - 4u)];
  u32 img3 = (map < 4u) ? f->bp[0x94u + map] : f->bp[0xB4u + (map - 4u)];
  if (!img0 && !img3)
    return NULL;
  int w = (int)(img0 & 0x3FFu) + 1;
  int h = (int)((img0 >> 10) & 0x3FFu) + 1;
  u32 fmt = (img0 >> 20) & 0xFu;
  u32 addr = (img3 & 0x00FFFFFFu) << 5;
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024)
    return NULL;

  for (unsigned i = 0; i < g->tex_n; i++) {
    Tex *t = &g->tex[i];
    if (t->addr == addr && t->fmt == fmt && t->w == w && t->h == h)
      return t->px ? t : NULL;
  }
  if (g->tex_n >= TEX_CACHE)
    return NULL;

  Tex *t = &g->tex[g->tex_n++];
  t->addr = addr;
  t->fmt = fmt;
  t->w = w;
  t->h = h;
  t->px = NULL;

  int bw, bh, bytes;
  if (!tile_geom(fmt, &bw, &bh, &bytes)) {
    g->tex_unsupported++;
    return NULL;
  }
  u32 need = (u32)((w + bw - 1) / bw) * (u32)((h + bh - 1) / bh) * (u32)bytes;
  const u8 *src = f->mem_fn ? f->mem_fn(f->mem_user, addr & 0x3FFFFFFFu, need)
                            : NULL;
  if (!src) {
    g->tex_unmapped++;
    return NULL;
  }
  u8 *px = (u8 *)calloc((size_t)w * (size_t)h, 4u);
  if (!px)
    return NULL;
  if (!decode_texture(src, need, fmt, w, h, px)) {
    free(px);
    g->tex_unsupported++;
    return NULL;
  }
  t->px = px;
  g->tex_decoded++;
  return t;
}

static int wrap_coord(int v, int n, u32 mode) {
  if (n <= 0)
    return 0;
  switch (mode & 3u) {
  case 1: /* repeat */
    v %= n;
    return v < 0 ? v + n : v;
  case 2: { /* mirror */
    int period = 2 * n;
    v %= period;
    if (v < 0)
      v += period;
    return v < n ? v : period - 1 - v;
  }
  default: /* clamp */
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
  }
}

static void tex_sample(SoftGfx *g, const Tex *t, float u, float v, u8 *out) {
  u32 mode0 = (g->fifo->bp[0x80u] & 0x00FFFFFFu);
  int x = wrap_coord((int)floorf(u * (float)t->w), t->w, mode0);
  int y = wrap_coord((int)floorf(v * (float)t->h), t->h, mode0 >> 2);
  memcpy(out, t->px + ((size_t)y * (size_t)t->w + (size_t)x) * 4u, 4);
}

/* BP 0xF3: two comparisons against two references, combined by a logic op. */
static int alpha_test(const GxRegs *f, u32 a) {
  u32 reg = f->bp[0xF3u];
  u32 ref0 = reg & 0xFFu, ref1 = (reg >> 8) & 0xFFu;
  u32 c0 = (reg >> 16) & 7u, c1 = (reg >> 19) & 7u, op = (reg >> 22) & 3u;
  int r0, r1;
  switch (c0) {
  case 0: r0 = 0; break;
  case 1: r0 = a < ref0; break;
  case 2: r0 = a == ref0; break;
  case 3: r0 = a <= ref0; break;
  case 4: r0 = a > ref0; break;
  case 5: r0 = a != ref0; break;
  case 6: r0 = a >= ref0; break;
  default: r0 = 1; break;
  }
  switch (c1) {
  case 0: r1 = 0; break;
  case 1: r1 = a < ref1; break;
  case 2: r1 = a == ref1; break;
  case 3: r1 = a <= ref1; break;
  case 4: r1 = a > ref1; break;
  case 5: r1 = a != ref1; break;
  case 6: r1 = a >= ref1; break;
  default: r1 = 1; break;
  }
  switch (op) {
  case 0: return r0 && r1;
  case 1: return r0 || r1;
  case 2: return r0 ^ r1;
  default: return !(r0 ^ r1);
  }
}

/* Per-pixel blend factors, BP 0x41 bits 5-7 (dst) and 8-10 (src). */
static float blend_factor(u32 sel, const float *src, const float *dst) {
  switch (sel & 7u) {
  case 0:
    return 0.0f;
  case 1:
    return 1.0f;
  case 2:
    return dst[0]; /* per-channel; the caller overrides for colour factors */
  case 3:
    return 1.0f - dst[0];
  case 4:
    return src[3];
  case 5:
    return 1.0f - src[3];
  case 6:
    return dst[3];
  default:
    return 1.0f - dst[3];
  }
}

static float edge(const SVert *a, const SVert *b, float px, float py) {
  return (px - a->x) * (b->y - a->y) - (py - a->y) * (b->x - a->x);
}

static void raster_screen(SoftGfx *g, const SVert *a, const SVert *b,
                       const SVert *c) {
  if (!a->ok || !b->ok || !c->ok)
    return;
  int is_persp = !(g->fifo->xf[XF_PROJECTION + 6] & 1u);
  float minx = fminf(a->x, fminf(b->x, c->x));
  float maxx = fmaxf(a->x, fmaxf(b->x, c->x));
  float miny = fminf(a->y, fminf(b->y, c->y));
  float maxy = fmaxf(a->y, fmaxf(b->y, c->y));
  int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
  int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
  if (x0 < 0)
    x0 = 0;
  if (y0 < 0)
    y0 = 0;
  if (x1 > g->w)
    x1 = g->w;
  if (y1 > g->h)
    y1 = g->h;
  if (x0 >= x1 || y0 >= y1) {
    if (is_persp && g->ttrace) {
      const GxRegs *ff = g->fifo;
      g->ttrace--;
      printf("off tri [%.0f %.0f][%.0f %.0f][%.0f %.0f] vp=[%.1f %.1f %.1f "
             "%.1f %.1f %.1f] blend=%06x\n",
             a->x, a->y, b->x, b->y, c->x, c->y, xf_f(ff, XF_VIEWPORT + 0),
             xf_f(ff, XF_VIEWPORT + 1), xf_f(ff, XF_VIEWPORT + 2),
             xf_f(ff, XF_VIEWPORT + 3), xf_f(ff, XF_VIEWPORT + 4),
             xf_f(ff, XF_VIEWPORT + 5), ff->bp[0x41]);
    }
    g->offscreen++;
    if (is_persp)
      g->offscreen_persp++;
    return;
  }
  float area = edge(a, b, c->x, c->y);
  if (area == 0.0f) {
    g->degenerate++;
    if (is_persp)
      g->degen_persp++;
    return;
  }
  /* Both windings are accepted: back-face culling lives in BP 0x40 and is not
   * modelled yet, and dropping half the triangles hides more than it helps. */
  float inv = 1.0f / area;
  g->tris_drawn++;
  if (is_persp)
    g->drawn_persp++;

  /* ZMODE (BP 0x40): bit 0 test enable, bits 1-3 compare function, bit 4
   * depth update. BLENDMODE (BP 0x41): bit 0 blend enable, bit 3 colour
   * update, bits 5-7 destination factor, bits 8-10 source factor. Without
   * these a 2D overlay drawn last paints over the whole frame. */
  u32 zmode = g->fifo->bp[0x40];
  u32 bmode = g->fifo->bp[0x41];
  int ztest = (int)(zmode & 1u);
  u32 zfunc = (zmode >> 1) & 7u;
  int zwrite = (int)((zmode >> 4) & 1u);
  int blend = (int)(bmode & 1u);
  int cupdate = (int)((bmode >> 3) & 1u);
  u32 dfac = (bmode >> 5) & 7u;
  u32 sfac = (bmode >> 8) & 7u;
  if (!cupdate && is_persp)
    g->p_nocolor++;
  if (!cupdate && !zwrite)
    return;

  /* Scissor: BP 0x20/0x21 hold the rectangle biased by 342, BP 0x59 the
   * offset the copy applies. */
  {
    u32 tl = g->fifo->bp[0x20], br = g->fifo->bp[0x21];
    int sy0 = (int)(tl & 0x7FFu) - 342;
    int sx0 = (int)((tl >> 12) & 0x7FFu) - 342;
    int sy1 = (int)(br & 0x7FFu) - 342 + 1;
    int sx1 = (int)((br >> 12) & 0x7FFu) - 342 + 1;
    if (sx0 > x0)
      x0 = sx0;
    if (sy0 > y0)
      y0 = sy0;
    if (sx1 < x1)
      x1 = sx1;
    if (sy1 < y1)
      y1 = sy1;
    if (x0 < 0)
      x0 = 0;
    if (y0 < 0)
      y0 = 0;
    if (x0 >= x1 || y0 >= y1) {
      g->scissored++;
      if (is_persp)
        g->p_scissor++;
      return;
    }
  }

  /* Texture map 0 only for now: SMG's UI and logo passes bind one map, and
   * multi-map compositing needs real TEV stages, not a wider sampler. */
  const Tex *tex = NULL;
  if (!g->no_tex && (g->fifo->bp[0x00] & 0xFu) != 0u)
    tex = tex_bind(g, 0u);

  for (int y = y0; y < y1; y++) {
    float py = (float)y + 0.5f;
    for (int x = x0; x < x1; x++) {
      float px = (float)x + 0.5f;
      float w0 = edge(b, c, px, py) * inv;
      float w1 = edge(c, a, px, py) * inv;
      float w2 = edge(a, b, px, py) * inv;
      if ((w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) &&
          (w0 > 0.0f || w1 > 0.0f || w2 > 0.0f))
        continue;

      /* Shade first: the alpha test gates the depth write too, so it has to
       * run before the z buffer is touched. */
      float src[4], dst[4];
      for (int k = 0; k < 3; k++)
        src[k] = (w0 * a->c[k] + w1 * b->c[k] + w2 * c->c[k]) * (1.0f / 255.0f);
      src[3] = (w0 * a->c[3] + w1 * b->c[3] + w2 * c->c[3]) * (1.0f / 255.0f);
      if (tex) {
        /* Texcoords interpolate in 1/w so they do not swim under
         * perspective; for the ortho UI passes w is 1 and this is exact. */
        float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
        u8 texel[4] = {255, 255, 255, 255};
        if (iw > 0.0f) {
          float tu =
              (w0 * a->u * a->iw + w1 * b->u * b->iw + w2 * c->u * c->iw) / iw;
          float tv =
              (w0 * a->v * a->iw + w1 * b->v * b->iw + w2 * c->v * c->iw) / iw;
          tex_sample(g, tex, tu, tv, texel);
        }
        /* Standing in for TEV: modulate the texture by the raster colour.
         * Generic TEV stage translation is the next milestone. */
        for (int k = 0; k < 4; k++)
          src[k] *= (float)texel[k] * (1.0f / 255.0f);
        g->tex_pixels++;
      }
      {
        float af = src[3] * 255.0f;
        u32 a8 = (u32)(af <= 0.0f ? 0.0f : (af >= 255.0f ? 255.0f : af));
        if (!alpha_test(g->fifo, a8)) {
          g->alpha_killed++;
          continue;
        }
      }

      float z = w0 * a->z + w1 * b->z + w2 * c->z;
      size_t idx = (size_t)y * (size_t)g->w + (size_t)x;
      float zb = g->depth[idx];
      if (ztest) {
        int pass;
        switch (zfunc) {
        case 0: pass = 0; break;
        case 1: pass = z < zb; break;
        case 2: pass = z == zb; break;
        case 3: pass = z <= zb; break;
        case 4: pass = z > zb; break;
        case 5: pass = z != zb; break;
        case 6: pass = z >= zb; break;
        default: pass = 1; break;
        }
        if (!pass) {
          if (is_persp)
            g->p_zfail++;
          continue;
        }
      }
      if (zwrite)
        g->depth[idx] = z;
      if (!cupdate)
        continue;

      u8 *px4 = g->color + idx * 4u;
      for (int k = 0; k < 4; k++)
        dst[k] = px4[k] * (1.0f / 255.0f);
      for (int k = 0; k < 3; k++) {
        float out;
        if (blend) {
          float sf = blend_factor(sfac, src, dst);
          float df = blend_factor(dfac, src, dst);
          if ((sfac & 7u) == 2u)
            sf = dst[k];
          else if ((sfac & 7u) == 3u)
            sf = 1.0f - dst[k];
          if ((dfac & 7u) == 2u)
            df = src[k];
          else if ((dfac & 7u) == 3u)
            df = 1.0f - src[k];
          out = src[k] * sf + dst[k] * df;
        } else {
          out = src[k];
        }
        px4[k] = (u8)(out <= 0.0f ? 0 : out >= 1.0f ? 255 : out * 255.0f + 0.5f);
      }
      px4[3] = 0xFF;
      g->pixels++;
      if (is_persp)
        g->pixels_persp++;
    }
  }
}

/* Assembly entry: clip the triangle against the near plane, then project.
 * Without this a triangle straddling the eye plane divides by a w near zero
 * and smears across the frame -- most of SMG's title-screen skybox does
 * exactly that. */
static void raster_tri(SoftGfx *g, const SVert *a, const SVert *b,
                       const SVert *c) {
  g->tris_in++;
  if (!a->ok || !b->ok || !c->ok)
    return;

  const SVert *in[3];
  in[0] = a;
  in[1] = b;
  in[2] = c;

  SVert poly[4];
  int n = 0;
  for (int i = 0; i < 3; i++) {
    const SVert *cur = in[i];
    const SVert *nxt = in[(i + 1) % 3];
    /* GX clip space is -w <= z <= 0, so the near plane is z + w = 0. Clipping
     * on w > 0 alone lets through points between the eye and the near plane,
     * which divide by a tiny w and project to infinity. */
    float dc = cur->z + cur->w;
    float dn = nxt->z + nxt->w;
    int cur_in = dc > 0.0f;
    int nxt_in = dn > 0.0f;
    if (cur_in)
      poly[n++] = *cur;
    if (cur_in != nxt_in) {
      float t = dc / (dc - dn);
      poly[n++] = lerp_vert(cur, nxt, t);
    }
  }
  if (n < 3) {
    g->clipped_out++;
    return;
  }
  for (int i = 0; i < n; i++)
    project(g, &poly[i]);
  raster_screen(g, &poly[0], &poly[1], &poly[2]);
  if (n == 4)
    raster_screen(g, &poly[0], &poly[2], &poly[3]);
}

/* ---------------------------------------------------------------------- */
/* Sink.                                                                   */

/* Writes the CP/XF/BP register files as one blob the host shader probe can
 * replay into Dolphin's generators. CP is reconstructed into hardware register
 * numbering (0x50 VCD lo, 0x60 VCD hi, 0x70/0x80/0x90 + i the three VAT
 * words), which is what VideoCommon indexes by. */
static void state_dump(SoftGfx *g, u8 prim) {
  const GxRegs *f = g->fifo;
  u64 h = 1469598103934665603ull;
  u32 cp[256];
  u32 hdr[6];
  char path[320];
  FILE *fp;

  /* Identity is the state a pixel shader is generated from: general mode (TEV
   * stage and texgen counts), the TEV stages themselves, alpha compare, fog,
   * and the texture mode/format registers. */
  for (unsigned i = 0xC0u; i <= 0xDFu; i++) { h ^= f->bp[i]; h *= 1099511628211ull; }
  for (unsigned i = 0x80u; i <= 0x97u; i++) { h ^= f->bp[i]; h *= 1099511628211ull; }
  for (unsigned i = 0xEEu; i <= 0xF3u; i++) { h ^= f->bp[i]; h *= 1099511628211ull; }
  h ^= f->bp[0x00]; h *= 1099511628211ull;
  h ^= f->vcd_low;  h *= 1099511628211ull;
  h ^= f->vcd_high; h *= 1099511628211ull;

  for (int i = 0; i < g->st_n; i++)
    if (g->st_seen[i] == h)
      return;
  if (g->st_n == (int)(sizeof(g->st_seen) / sizeof(g->st_seen[0])))
    return;
  g->st_seen[g->st_n++] = h;

  memset(cp, 0, sizeof(cp));
  cp[0x50] = f->vcd_low;
  cp[0x60] = f->vcd_high;
  for (unsigned i = 0; i < 8u; i++) {
    cp[0x70u + i] = f->vat[i][0];
    cp[0x80u + i] = f->vat[i][1];
    cp[0x90u + i] = f->vat[i][2];
  }

  snprintf(path, sizeof(path), "%s/state_%016llx.bin", g->st_dir,
           (unsigned long long)h);
  fp = fopen(path, "wb");
  if (!fp)
    return;
  hdr[0] = 0x54535647u; /* "GVST" */
  hdr[1] = 1u;
  hdr[2] = prim;
  hdr[3] = 256u;
  hdr[4] = GX_XF_WORDS;
  hdr[5] = 256u;
  fwrite(hdr, sizeof(hdr), 1, fp);
  fwrite(cp, sizeof(u32), 256, fp);
  fwrite(f->xf, sizeof(u32), GX_XF_WORDS, fp);
  fwrite(f->bp, sizeof(u32), 256, fp);
  fclose(fp);
  g->st_written++;
}

static void on_prim_begin(void *user, u8 prim, u32 count) {
  SoftGfx *g = (SoftGfx *)user;
  (void)count;
  g->prim = prim;
  if (g->st_dir[0])
    state_dump(g, prim);
  g->in_prim = 0;
  g->parity = 0;
  g->prim_index++;
}

static void on_vertex(void *user, const GxVertex *v) {
  SoftGfx *g = (SoftGfx *)user;
  SVert s = transform(g, v);
  g->verts++;
  u8 kind = g->prim & 0xF8u;

  if (kind == 0x98u) { /* triangle strip */
    if (g->in_prim >= 2) {
      if (g->parity)
        raster_tri(g, &g->prev2, &g->prev1, &s);
      else
        raster_tri(g, &g->prev1, &g->prev2, &s);
      g->parity ^= 1;
    }
    g->prev2 = g->prev1;
    g->prev1 = s;
    g->in_prim++;
    return;
  }
  if (kind == 0xA0u) { /* triangle fan */
    if (g->in_prim == 0)
      g->fan0 = s;
    else if (g->in_prim >= 2)
      raster_tri(g, &g->fan0, &g->prev1, &s);
    g->prev1 = s;
    g->in_prim++;
    return;
  }
  if (kind == 0x90u) { /* independent triangles */
    if (g->in_prim % 3u == 2u)
      raster_tri(g, &g->prev2, &g->prev1, &s);
    g->prev2 = g->prev1;
    g->prev1 = s;
    g->in_prim++;
    return;
  }
  if (kind == 0x80u || kind == 0x88u) { /* quads */
    u32 k = g->in_prim % 4u;
    if (k == 0)
      g->fan0 = s;
    else if (k == 3) {
      raster_tri(g, &g->fan0, &g->prev2, &g->prev1);
      raster_tri(g, &g->fan0, &g->prev1, &s);
    }
    g->prev2 = g->prev1;
    g->prev1 = s;
    g->in_prim++;
    return;
  }
  /* lines and points are not drawn yet */
  g->in_prim++;
}

static void on_prim_end(void *user) {
  SoftGfx *g = (SoftGfx *)user;
  g->in_prim = 0;
}

static void clear_efb(SoftGfx *g) {
  u32 ar = g->fifo->bp[0x4F];
  u32 gb = g->fifo->bp[0x50];
  u8 rgba[4];
  rgba[0] = (u8)(ar & 0xFFu);
  rgba[1] = (u8)((gb >> 8) & 0xFFu);
  rgba[2] = (u8)(gb & 0xFFu);
  rgba[3] = (u8)((ar >> 8) & 0xFFu);
  for (size_t i = 0; i < (size_t)g->w * (size_t)g->h; i++) {
    memcpy(g->color + i * 4u, rgba, 4);
    g->depth[i] = 1e30f;
  }
}

static void on_copy(void *user, u32 data) {
  SoftGfx *g = (SoftGfx *)user;
  g->copies++;
  if (g->trace > 0) {
    const GxRegs *f = g->fifo;
    g->trace--;
    printf("gfx copy data=%06x vp=[%.2f %.2f %.2f %.2f %.2f %.2f] "
           "proj=[%.4f %.4f %.4f %.4f %.4f %.4f] ortho=%u src=%06x/%06x\n",
           data, xf_f(f, XF_VIEWPORT + 0), xf_f(f, XF_VIEWPORT + 1),
           xf_f(f, XF_VIEWPORT + 2), xf_f(f, XF_VIEWPORT + 3),
           xf_f(f, XF_VIEWPORT + 4), xf_f(f, XF_VIEWPORT + 5),
           xf_f(f, XF_PROJECTION + 0), xf_f(f, XF_PROJECTION + 1),
           xf_f(f, XF_PROJECTION + 2), xf_f(f, XF_PROJECTION + 3),
           xf_f(f, XF_PROJECTION + 4), xf_f(f, XF_PROJECTION + 5),
           f->xf[XF_PROJECTION + 6], f->bp[0x49], f->bp[0x4A]);
  }
  /* Bit 14 is copy-to-XFB: that is a finished frame. Everything else is a
   * render-to-texture copy, which must not be mistaken for one. */
  int to_xfb = (data >> 14) & 1u;
  if (to_xfb) {
    g->copies_xfb++;
    if (g->dir[0] && (int)(g->frames / g->stride) < g->max_frames &&
        (g->frames % g->stride) == 0u) {
      u32 wh = g->fifo->bp[0x4A];
      int cw = (int)(wh & 0x3FFu) + 1;
      int chh = (int)((wh >> 10) & 0x3FFu) + 1;
      if (cw <= 0 || cw > g->w)
        cw = g->w;
      if (chh <= 0 || chh > g->h)
        chh = g->h;
      char path[320];
      snprintf(path, sizeof(path), "%s/frame_%04u.png", g->dir, g->frames);
      u8 *crop = (u8 *)malloc((size_t)cw * (size_t)chh * 4u);
      if (crop) {
        for (int y = 0; y < chh; y++)
          memcpy(crop + (size_t)y * (size_t)cw * 4u,
                 g->color + (size_t)y * (size_t)g->w * 4u, (size_t)cw * 4u);
        png_write(path, crop, cw, chh);
        free(crop);
      }
    }
    g->frames++;
    /* SMG's display copy does not set the clear bit, so nothing would ever
     * reset the EFB and every frame would accumulate onto the last. Clearing
     * after the frame has been captured gives one image per presented frame,
     * which is what makes these dumps readable. */
    clear_efb(g);
  }
  /* Bit 11 clears the EFB as part of the copy. */
  if ((data >> 11) & 1u)
    clear_efb(g);
}

/* ---------------------------------------------------------------------- */

SoftGfx *soft_gfx_create(const GxRegs *fifo, const char *out_dir, int max_frames) {
  SoftGfx *g = (SoftGfx *)calloc(1, sizeof(*g));
  if (!g)
    return NULL;
  g->fifo = fifo;
  g->w = EFB_W;
  g->h = EFB_H;
  g->color = (u8 *)malloc((size_t)g->w * (size_t)g->h * 4u);
  g->depth = (float *)malloc((size_t)g->w * (size_t)g->h * sizeof(float));
  g->max_frames = max_frames;
  { const char *nt = getenv("GV_GFX_NOTEX"); g->no_tex = nt && *nt != '0';
    const char *vt = getenv("GV_GFX_VTRACE"); g->vtrace = vt ? (u32)atoi(vt) : 0u;
    const char *tt = getenv("GV_GFX_TTRACE"); g->ttrace = tt ? (u32)atoi(tt) : 0u;
    const char *sd = getenv("GV_GFX_STATEDUMP");
    if (sd && *sd) snprintf(g->st_dir, sizeof(g->st_dir), "%s", sd); }
  {
    const char *sh = getenv("GV_GFX_SHADE");
    const char *ev = getenv("GV_GFX_EVERY");
    g->shade_prim = sh && strcmp(sh, "prim") == 0;
    g->stride = ev ? (u32)strtoul(ev, NULL, 10) : 1u;
    {
      const char *tr = getenv("GV_GFX_TRACE");
      g->trace = tr ? atoi(tr) : 0;
    }
    if (!g->stride)
      g->stride = 1u;
  }
  if (!g->color || !g->depth) {
    soft_gfx_destroy(g);
    return NULL;
  }
  if (out_dir)
    snprintf(g->dir, sizeof(g->dir), "%s", out_dir);
  clear_efb(g);
  return g;
}

void soft_gfx_sink(SoftGfx *g, GxSink *out) {
  memset(out, 0, sizeof(*out));
  out->user = g;
  out->prim_begin = on_prim_begin;
  out->vertex = on_vertex;
  out->prim_end = on_prim_end;
  out->copy = on_copy;
}

void soft_gfx_report(const SoftGfx *g) {
  printf("gfx verts=%u tris=%u drawn=%u degenerate=%u no_matrix=%u "
         "behind=%u no_viewport=%u\n",
         g->verts, g->tris_in, g->tris_drawn, g->degenerate, g->no_matrix,
         g->v_behind, g->v_novp);
  printf("gfx offscreen=%u (persp %u)\n", g->offscreen, g->offscreen_persp);
  if (g->st_dir[0])
    printf("gfx statedump=%d distinct draw states\n", g->st_written);
  printf("gfx persp reject: nocolor=%u scissor=%u zfail=%llu\n", g->p_nocolor,
         g->p_scissor, (unsigned long long)g->p_zfail);
  printf("gfx clipped_out=%u persp drawn=%u degen=%u pixels=%llu\n",
         g->clipped_out, g->drawn_persp, g->degen_persp,
         (unsigned long long)g->pixels_persp);
  printf("gfx tex decoded=%u unmapped=%u unsupported=%u pixels=%llu "
         "alpha_killed=%u\n",
         g->tex_decoded, g->tex_unmapped, g->tex_unsupported,
         (unsigned long long)g->tex_pixels, g->alpha_killed);
  printf("gfx scissored=%u pixels=%llu\n", g->scissored,
         (unsigned long long)g->pixels);
  printf("gfx persp=%u ortho=%u\n", g->persp_verts, g->ortho_verts);
  printf("gfx copies=%u to_xfb=%u frames_written=%u\n", g->copies,
         g->copies_xfb, g->frames);
}

void soft_gfx_destroy(SoftGfx *g) {
  if (!g)
    return;
  for (unsigned i = 0; i < g->tex_n; i++)
    free(g->tex[i].px);
  free(g->color);
  free(g->depth);
  free(g);
}

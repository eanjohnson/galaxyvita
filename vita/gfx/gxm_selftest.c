/* Proves the sceGxm path on the hardware that has to run it, without needing
 * the game.
 *
 * The question this answers is narrow and was not answerable on the host: does
 * a program the console's own Cg compiler produced bind through
 * sceGxmShaderPatcher, take GX clip-space vertices, and land the right pixels
 * in the right places? Everything upstream of that -- the FIFO decode, the
 * vertex loader, the transform -- is already checked against the host
 * rasteriser frame by frame.
 *
 * So this drives gx_gxm.c's sink directly with a synthetic register file whose
 * projection and viewport are chosen to make the arithmetic checkable by eye:
 * object space is the unit square, the EFB is 640x448, and each primitive
 * covers a rectangle whose corners are exact.
 *
 * Writes: ux0:data/galaxyvita/gxm_selftest.txt   the report
 *         ux0:data/galaxyvita/gxm_frame.raw      960x544 RGBA, tight
 *
 * Runs unattended: no input, exits when done. */

#include "gx_gxm.h"
#include "gxm.h"
#include "shader_cache.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPORT "ux0:data/galaxyvita/gxm_selftest.txt"
#define FRAME "ux0:data/galaxyvita/gxm_frame.raw"
#define TEXFRAME_FMT "ux0:data/galaxyvita/gxm_texframe%u.raw"
#define MARK "ux0:data/galaxyvita/gxm_batch.txt"

/* ShaccCg is the reason for the large heap: it churns tens of megabytes per
 * compile and how many compiles survive in one process scales with heap size.
 * Only two programs are needed here, so this is comfortable. */
unsigned int _newlib_heap_size_user = 192 * 1024 * 1024;

enum {
  XF_VIEWPORT = 0x101Au,
  XF_PROJECTION = 0x1020u
};

enum { EFB_W = 640, EFB_H = 448 };

static SceUID g_report;

static void out(const char *fmt, ...) {
  char line[512];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n > 0)
    sceIoWrite(g_report, line, (SceSize)n);
}

static GxRegs g_regs;

static void put_f(unsigned addr, float value) {
  union {
    unsigned u;
    float f;
  } bits;
  bits.f = value;
  if (addr < GX_XF_WORDS)
    g_regs.xf[addr] = bits.u;
}

/* An orthographic unit square mapped onto a 640x448 EFB, with the identity as
 * the position matrix. Object (0,0) is the bottom-left of the EFB and (1,1) the
 * top-right, so every expected pixel below can be read straight off the
 * vertex coordinates. */
static void build_registers(void) {
  int r, c;
  memset(&g_regs, 0, sizeof(g_regs));

  for (r = 0; r < 3; r++)
    for (c = 0; c < 4; c++)
      put_f((unsigned)(r * 4 + c), r == c ? 1.0f : 0.0f);

  put_f(XF_VIEWPORT + 0, (float)EFB_W / 2.0f);  /* wd/2 */
  put_f(XF_VIEWPORT + 1, -(float)EFB_H / 2.0f); /* -ht/2 */
  put_f(XF_VIEWPORT + 2, 0.5f);                 /* z range */
  put_f(XF_VIEWPORT + 3, 342.0f + (float)EFB_W / 2.0f);
  put_f(XF_VIEWPORT + 4, 342.0f + (float)EFB_H / 2.0f);
  put_f(XF_VIEWPORT + 5, 0.5f); /* far z */

  /* Orthographic: clip = p0*e + p1, so the unit square lands on [-1,1]. */
  put_f(XF_PROJECTION + 0, 2.0f);
  put_f(XF_PROJECTION + 1, -1.0f);
  put_f(XF_PROJECTION + 2, 2.0f);
  put_f(XF_PROJECTION + 3, -1.0f);
  put_f(XF_PROJECTION + 4, 1.0f);
  put_f(XF_PROJECTION + 5, 0.0f);
  g_regs.xf[XF_PROJECTION + 6] = 1u; /* orthographic */

  /* The copy rectangle the frame is scaled from: width-1 | (height-1) << 10. */
  g_regs.bp[0x4A] = (unsigned)(EFB_W - 1) | ((unsigned)(EFB_H - 1) << 10);

  /* The copy-clear colour the backend now reads rather than hardcoding:
   * 0x4F is alpha:red and 0x50 is green:blue. The probe below still expects
   * 0D1A29, so this keeps the existing expectation and exercises the path
   * that supplies it. */
  g_regs.bp[0x4F] = (0xFFu << 8) | 0x0Du;
  g_regs.bp[0x50] = (0x1Au << 8) | 0x29u;
}

/* A synthetic texture and the registers that bind it.
 *
 * The console draws the game's strap screen as a white rectangle from a
 * texture it decodes perfectly, at texcoords measured to span 0..1, through a
 * sampler bound without error -- so tex2D returns white and nothing upstream
 * explains it. Reproducing that here removes the emulator, the FIFO, the
 * vertex loader and four minutes of guest time from each attempt, and leaves
 * only the part that is actually in question.
 *
 * Four flat quadrants rather than a gradient: a sample that lands in the wrong
 * place names which corner it came from, and a sample that returns the
 * unbound-unit default is white, which is none of them. */
enum { TEX_W = 64, TEX_H = 64, TEX_ADDR = 0x00300000u };

static unsigned char g_tex_tiled[TEX_W * TEX_H * 2];

static const unsigned char k_quadrant[4][3] = {
    {0xFF, 0x20, 0x20}, /* top-left     red    */
    {0x20, 0xFF, 0x20}, /* top-right    green  */
    {0x20, 0x20, 0xFF}, /* bottom-left  blue   */
    {0xFF, 0xFF, 0x20}  /* bottom-right yellow */
};

/* RGB565, whose tile is 4x4 texels in 32 bytes, stored big-endian. Written
 * tiled rather than linear because that is the layout gx_tex_decode() has to
 * undo, and a decoder fed an untiled image would pass for the wrong reason. */
static void build_texture(void) {
  int x, y;
  for (y = 0; y < TEX_H; y++) {
    for (x = 0; x < TEX_W; x++) {
      const unsigned char *c =
          k_quadrant[((y >= TEX_H / 2) ? 2 : 0) + ((x >= TEX_W / 2) ? 1 : 0)];
      const unsigned v = (unsigned)((c[0] >> 3) << 11) |
                         (unsigned)((c[1] >> 2) << 5) | (unsigned)(c[2] >> 3);
      const int tile = (y / 4) * (TEX_W / 4) + (x / 4);
      const int in_tile = (y % 4) * 4 + (x % 4);
      unsigned char *o = g_tex_tiled + (tile * 32) + in_tile * 2;
      o[0] = (unsigned char)(v >> 8);
      o[1] = (unsigned char)(v & 0xFFu);
    }
  }
}

static const u8 *tex_mem(void *user, u32 phys, u32 len) {
  (void)user;
  if (phys != TEX_ADDR || len > sizeof(g_tex_tiled))
    return NULL;
  return g_tex_tiled;
}

/* GenMode's low nibble is the texture count; IMG0 carries size and format and
 * IMG3 the address in 32-byte units. Format 4 is RGB565. */
static void bind_test_texture(void) {
  g_regs.mem_fn = tex_mem;
  g_regs.mem_user = NULL;
  g_regs.bp[0x00] = (g_regs.bp[0x00] & ~0xFu) | 1u;
  g_regs.bp[0x88] = (unsigned)(TEX_W - 1) | ((unsigned)(TEX_H - 1) << 10) |
                    (4u << 20);
  g_regs.bp[0x94] = TEX_ADDR >> 5;
  g_regs.bp[0x80] = 0u; /* clamp in both axes */
}

static void unbind_test_texture(void) { g_regs.bp[0x00] &= ~0xFu; }

static GxSink g_sink;

static void vtx(float x, float y, unsigned char r, unsigned char gg,
                unsigned char b) {
  GxVertex v;
  memset(&v, 0, sizeof(v));
  v.pos[0] = x;
  v.pos[1] = y;
  v.pos[2] = 0.0f;
  v.color0[0] = r;
  v.color0[1] = gg;
  v.color0[2] = b;
  v.color0[3] = 0xFF;
  v.has_color0 = 1;
  g_sink.vertex(g_sink.user, &v);
}

/* The scene: three primitives with different topologies, each a solid colour
 * in its own quarter of the frame, so a wrong topology shows up as a missing
 * or misshapen block rather than as a subtle shading difference. */
static void draw_scene(void) {
  g_sink.prim_begin(g_sink.user, 0x90u, 3); /* triangles */
  vtx(0.05f, 0.05f, 0xFF, 0x30, 0x30);
  vtx(0.45f, 0.05f, 0xFF, 0x30, 0x30);
  vtx(0.25f, 0.45f, 0xFF, 0x30, 0x30);
  g_sink.prim_end(g_sink.user);

  g_sink.prim_begin(g_sink.user, 0x80u, 4); /* quads */
  vtx(0.55f, 0.05f, 0x30, 0xFF, 0x30);
  vtx(0.95f, 0.05f, 0x30, 0xFF, 0x30);
  vtx(0.95f, 0.45f, 0x30, 0xFF, 0x30);
  vtx(0.55f, 0.45f, 0x30, 0xFF, 0x30);
  g_sink.prim_end(g_sink.user);

  g_sink.prim_begin(g_sink.user, 0x98u, 4); /* triangle strip */
  vtx(0.05f, 0.55f, 0x40, 0x60, 0xFF);
  vtx(0.05f, 0.95f, 0x40, 0x60, 0xFF);
  vtx(0.45f, 0.55f, 0x40, 0x60, 0xFF);
  vtx(0.45f, 0.95f, 0x40, 0x60, 0xFF);
  g_sink.prim_end(g_sink.user);

  g_sink.prim_begin(g_sink.user, 0xA0u, 4); /* triangle fan */
  vtx(0.75f, 0.75f, 0xFF, 0xD0, 0x40);
  vtx(0.55f, 0.55f, 0xFF, 0xD0, 0x40);
  vtx(0.95f, 0.55f, 0xFF, 0xD0, 0x40);
  vtx(0.95f, 0.95f, 0xFF, 0xD0, 0x40);
  g_sink.prim_end(g_sink.user);

  /* Bit 14 is copy-to-XFB, which is what the sink treats as end of frame. */
  g_sink.copy(g_sink.user, 1u << 14);
}

/* A textured vertex. Same as vtx() with texcoords and a white raster colour,
 * so the modulate in the fragment shader is the identity and whatever reaches
 * the screen is the sampled texel unaltered. */
static void vtx_t(float x, float y, float u, float v) {
  GxVertex vv;
  memset(&vv, 0, sizeof(vv));
  vv.pos[0] = x;
  vv.pos[1] = y;
  vv.pos[2] = 0.0f;
  vv.tex0[0] = u;
  vv.tex0[1] = v;
  vv.color0[0] = vv.color0[1] = vv.color0[2] = vv.color0[3] = 0xFF;
  vv.has_color0 = 1;
  vv.has_tex0 = 1;
  g_sink.vertex(g_sink.user, &vv);
}

/* One textured quad over the whole unit square, in its own frame so the
 * untextured expectations above are unaffected. v runs top-down to match the
 * GX convention the game's own quads use. */
static void draw_textured_scene(void) {
  bind_test_texture();
  g_sink.prim_begin(g_sink.user, 0x80u, 4); /* quads */
  vtx_t(0.05f, 0.05f, 0.0f, 1.0f);
  vtx_t(0.95f, 0.05f, 1.0f, 1.0f);
  vtx_t(0.95f, 0.95f, 1.0f, 0.0f);
  vtx_t(0.05f, 0.95f, 0.0f, 0.0f);
  g_sink.prim_end(g_sink.user);
  g_sink.copy(g_sink.user, 1u << 14);
  unbind_test_texture();
}

/* Where each primitive should have landed, in EFB coordinates with y down,
 * converted to screen by the same pillarbox the sink applies. */
typedef struct {
  const char *what;
  float efb_x, efb_y;
  unsigned char r, g, b;
} Probe;

/* The textured frame. EFB y is down, and v was given top-down, so the texture's
 * top-left quadrant belongs at the top-left of the quad. */
static const Probe k_tex_probes[] = {
    {"tex top-left", 0.25f * EFB_W, 0.25f * EFB_H, 0xFF, 0x20, 0x20},
    {"tex top-right", 0.75f * EFB_W, 0.25f * EFB_H, 0x20, 0xFF, 0x20},
    {"tex bottom-left", 0.25f * EFB_W, 0.75f * EFB_H, 0x20, 0x20, 0xFF},
    {"tex bottom-right", 0.75f * EFB_W, 0.75f * EFB_H, 0xFF, 0xFF, 0x20},
};

static const Probe k_probes[] = {
    {"clear (corner)", 4.0f, 4.0f, 0x0D, 0x1A, 0x29},
    {"triangles", 0.25f * EFB_W, 0.80f * EFB_H, 0xFF, 0x30, 0x30},
    {"quads", 0.75f * EFB_W, 0.75f * EFB_H, 0x30, 0xFF, 0x30},
    {"strip", 0.25f * EFB_W, 0.25f * EFB_H, 0x40, 0x60, 0xFF},
    {"fan", 0.75f * EFB_W, 0.30f * EFB_H, 0xFF, 0xD0, 0x40},
};

static void run_probes(const unsigned char *frame, const Probe *probes,
                       unsigned n, float scale, float off_x, float off_y,
                       unsigned *passed, unsigned *failed) {
  unsigned i;
  for (i = 0; i < n; i++) {
    const Probe *p = &probes[i];
    int x = (int)(p->efb_x * scale + off_x);
    int y = (int)(p->efb_y * scale + off_y);
    const unsigned char *px;
    int ok;
    if (x < 0)
      x = 0;
    if (y < 0)
      y = 0;
    px = frame + ((size_t)y * gxm_width() + (size_t)x) * 4u;
    /* A8B8G8R8 in memory is R,G,B,A byte order. */
    ok = px[0] == p->r && px[1] == p->g && px[2] == p->b;
    out("  %-16s at %3d,%3d  got %02x%02x%02x  want %02x%02x%02x  %s\n", p->what,
        x, y, px[0], px[1], px[2], p->r, p->g, p->b, ok ? "ok" : "MISMATCH");
    if (ok)
      (*passed)++;
    else
      (*failed)++;
  }
}

int main(void) {
  unsigned char *frame;
  unsigned passed = 0, failed = 0;
  float scale, off_x, off_y;
  char line[256];

  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/galaxyvita", 0777);
  g_report = sceIoOpen(REPORT, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (g_report < 0) {
    sceKernelExitProcess(1);
    return 1;
  }

  if (!gxm_init()) {
    out("gxm_init failed: %s\n", gxm_error());
    goto done;
  }
  out("gxm up: %ux%u\n", gxm_width(), gxm_height());

  if (!gvsc_init("ux0:data/galaxyvita/gxp"))
    out("shader cache: %s\n", gvsc_last_error());

  build_registers();
  build_texture();
  if (!gx_gxm_init(&g_regs)) {
    out("gx_gxm_init failed: %s\n", gx_gxm_error());
    goto done;
  }
  {
    unsigned compiled = 0, cached = 0, bad = 0;
    gvsc_stats(&compiled, &cached, &bad);
    out("programs: compiled=%u cached=%u failed=%u\n", compiled, cached, bad);
  }
  gx_gxm_sink(&g_sink);

  /* Two frames, so that the second one exercises the arena rotation and the
   * second display buffer rather than only the first of each. */
  draw_scene();
  draw_scene();

  gx_gxm_report(line, sizeof(line));
  out("%s\n", line);

  frame = (unsigned char *)malloc((size_t)gxm_width() * gxm_height() * 4u);
  if (!frame || !gxm_capture(frame)) {
    out("capture failed\n");
    goto done;
  }
  {
    SceUID fd = sceIoOpen(FRAME, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
      sceIoWrite(fd, frame, (SceSize)(gxm_width() * gxm_height() * 4u));
      sceIoClose(fd);
    }
  }

  /* The sink pillarboxes a 640x448 frame onto 960x544; the probes have to use
   * the same mapping or they would sample the letterbox. */
  scale = (float)gxm_height() / (float)EFB_H;
  if ((float)gxm_width() / (float)EFB_W < scale)
    scale = (float)gxm_width() / (float)EFB_W;
  off_x = ((float)gxm_width() - (float)EFB_W * scale) * 0.5f;
  off_y = ((float)gxm_height() - (float)EFB_H * scale) * 0.5f;

  out("\nprobes (scale=%.4f offset=%.1f,%.1f)\n", (double)scale, (double)off_x,
      (double)off_y);
  run_probes(frame, k_probes, sizeof(k_probes) / sizeof(k_probes[0]), scale,
             off_x, off_y, &passed, &failed);

  /* The textured frame, drawn twice for the same arena-rotation reason. This
   * is the part the game cannot answer: the console samples a perfectly
   * decoded texture and gets white, with every stage between measured and
   * correct, so the question is whether GXM texturing works here at all. */
  /* Each texture-init strategy in turn, in one run.
   *
   * Getting to the console now costs a reboot's worth of risk, so testing one
   * hypothesis per upload is the wrong trade. The GPU has to be idle before
   * the cache is dropped, because the arena is reused from the start and a
   * scene still in flight is still reading it. */
  {
    unsigned mode;
    for (mode = 0; mode < 3u; mode++) {
      char path[192];
      unsigned mp = 0, mf = 0;
      gxm_finish();
      gx_gxm_set_tex_init_mode(mode);
      gx_gxm_flush_textures();
      draw_textured_scene();
      draw_textured_scene();
      gx_gxm_report(line, sizeof(line));
      out("\ninit mode %u: %s\n", mode, line);
      if (!gxm_capture(frame)) {
        out("  capture failed\n");
        continue;
      }
      snprintf(path, sizeof(path), TEXFRAME_FMT, mode);
      {
        SceUID fd =
            sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
          sceIoWrite(fd, frame, (SceSize)(gxm_width() * gxm_height() * 4u));
          sceIoClose(fd);
        }
      }
      run_probes(frame, k_tex_probes,
                 sizeof(k_tex_probes) / sizeof(k_tex_probes[0]), scale, off_x,
                 off_y, &mp, &mf);
      out("  init mode %u: %u ok, %u failed\n", mode, mp, mf);
      /* Only the working mode should count towards the overall result; a mode
       * that fails is an answer, not a regression. */
      if (mf == 0u)
        passed += mp;
    }
  }

  out("\ngxm_selftest: %u ok, %u failed\n", passed, failed);

done:
  out("gxm_selftest: done\n");
  sceIoClose(g_report);
  {
    SceUID fd = sceIoOpen(MARK, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
      sceIoWrite(fd, "done\n", 5);
      sceIoClose(fd);
    }
  }
  sceKernelExitProcess(0);
  return 0;
}

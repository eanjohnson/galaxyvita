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
}

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

/* Where each primitive should have landed, in EFB coordinates with y down,
 * converted to screen by the same pillarbox the sink applies. */
typedef struct {
  const char *what;
  float efb_x, efb_y;
  unsigned char r, g, b;
} Probe;

static const Probe k_probes[] = {
    {"clear (corner)", 4.0f, 4.0f, 0x0D, 0x1A, 0x29},
    {"triangles", 0.25f * EFB_W, 0.80f * EFB_H, 0xFF, 0x30, 0x30},
    {"quads", 0.75f * EFB_W, 0.75f * EFB_H, 0x30, 0xFF, 0x30},
    {"strip", 0.25f * EFB_W, 0.25f * EFB_H, 0x40, 0x60, 0xFF},
    {"fan", 0.75f * EFB_W, 0.30f * EFB_H, 0xFF, 0xD0, 0x40},
};

int main(void) {
  unsigned char *frame;
  unsigned i, passed = 0, failed = 0;
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
  for (i = 0; i < sizeof(k_probes) / sizeof(k_probes[0]); i++) {
    const Probe *p = &k_probes[i];
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
      passed++;
    else
      failed++;
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

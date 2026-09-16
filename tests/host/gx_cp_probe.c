/* Direct check of the command-processor swap: hand-build a FIFO stream and
 * confirm the decoded geometry comes out the sink the way the rasteriser
 * expects it. */
#include "gx_cp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_begin, g_end, g_verts;
static u8 g_prim;
static float g_pos[8][3];

static void on_begin(void *u, u8 prim, u32 count) {
  (void)u; (void)count; g_prim = prim; g_begin++;
}
static void on_vertex(void *u, const GxVertex *v) {
  (void)u;
  if (g_verts < 8) memcpy(g_pos[g_verts], v->pos, sizeof(v->pos));
  g_verts++;
}
static void on_end(void *u) { (void)u; g_end++; }
static void on_copy(void *u, u32 d) { (void)u; printf("copy=0x%06x\n", d); }

static void w8(GxCp *cp, u8 b) { gx_cp_write(cp, b, 1); }
static void w32(GxCp *cp, u32 v) { gx_cp_write(cp, v, 4); }

/* CP 0x08 reg,value */
static void cp_reg(GxCp *cp, u8 reg, u32 val) { w8(cp, 0x08); w8(cp, reg); w32(cp, val); }

int main(void) {
  void *mem1 = calloc(1, 0x1800000);
  GxCp *cp = gx_cp_create(mem1, 0x1800000, NULL, 0);
  GxSink sink;
  memset(&sink, 0, sizeof(sink));
  sink.prim_begin = on_begin; sink.vertex = on_vertex;
  sink.prim_end = on_end; sink.copy = on_copy;
  gx_cp_set_sink(cp, &sink);

  /* Direct float XYZ position only: VCD low bit9..10 = 1 (direct), VAT0
   * g0 bit0 = 1 (3 components), bits1..3 = 4 (float). */
  cp_reg(cp, 0x50, 1u << 9);
  cp_reg(cp, 0x60, 0);
  cp_reg(cp, 0x70, 1u | (4u << 1));

  printf("vertex_size_expect=12\n");

  /* FIFO opcode 0x48 = invalidate vertex cache. Distinct from BP register
   * 0x48 below. This is what GXInit emits and what deadlocked the first cut of
   * the swap: one class satisfying both GpuBackend and GxRenderDevice made the
   * override call itself forever. If this line returns, the split holds. */
  w8(cp, 0x48);
  printf("invalidate returned, unknown=%llu\n", (unsigned long long)gx_cp_unknown(cp));

  /* Triangles (0x90), 3 vertices, VAT 0. */
  w8(cp, 0x90); gx_cp_write(cp, 3, 2);
  const float xyz[9] = {0,0,0, 1,0,0, 0,1,0};
  for (int i = 0; i < 9; i++) {
    union { float f; u32 u; } c; c.f = xyz[i];
    w32(cp, c.u);
  }

  /* BP writes the flipper and rasteriser care about. */
  w8(cp, 0x61); w32(cp, (0x45u << 24) | 0x000002u);           /* SETDRAWDONE  */
  w8(cp, 0x61); w32(cp, (0x48u << 24) | 0x00BEEFu);           /* token + irq  */
  w8(cp, 0x61); w32(cp, (0x52u << 24) | 0x004000u);           /* copy execute */
  w8(cp, 0x48);                                               /* invalidate again */

  printf("begin=%d prim=0x%02x verts=%d end=%d\n", g_begin, g_prim, g_verts, g_end);
  for (int i = 0; i < g_verts && i < 3; i++)
    printf("  v%d = %.1f %.1f %.1f\n", i, g_pos[i][0], g_pos[i][1], g_pos[i][2]);
  printf("cmds=%llu draws=%llu vtx=%llu unknown=%llu pending=%u\n",
         (unsigned long long)gx_cp_commands(cp), (unsigned long long)gx_cp_draws(cp),
         (unsigned long long)gx_cp_vertices(cp), (unsigned long long)gx_cp_unknown(cp),
         gx_cp_pending(cp));

  u32 tok = 0;
  /* Sequenced one per statement: argument evaluation order is unspecified, so
   * reading `tok` in the same call that fills it is a coin flip. */
  int fin = gx_cp_take_finish(cp);
  int got = gx_cp_take_token(cp, &tok);
  int irq = gx_cp_take_token_irq(cp);
  printf("pe finish=%d token=%d(0x%04x) irq=%d\n", fin, got, tok, irq);
  printf("pe second take: finish=%d token=%d irq=%d (want 0 0 0)\n",
         gx_cp_take_finish(cp), gx_cp_take_token(cp, &tok),
         gx_cp_take_token_irq(cp));
  printf("regs bp[0x52]=0x%06x bp[0x45]=0x%06x\n",
         gx_cp_regs(cp)->bp[0x52], gx_cp_regs(cp)->bp[0x45]);
  gx_cp_destroy(cp);
  free(mem1);
  return 0;
}

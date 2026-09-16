/* Host-side chassis: load RMGE01 DOL, dispatch AOT until FIFO/cap. */
#include "chassis.h"
#include "gx_soft.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static void host_log(const char *msg, void *user) {
  (void)user;
  fprintf(stderr, "%s\n", msg);
}

/* A scripted player, so a headless run can show the game reacting to the
 * remote. GV_BTN is a Wii Remote button mask in the BT_BTN_* bit order; it is
 * held for GV_BTN_HOLD frames out of every GV_BTN_PERIOD, because a button the
 * game never sees released is not a press. GV_IR="x,y" parks the pointer, in
 * the camera's own 1024x768 units. */
typedef struct TestPad {
  u16 mask;
  u32 period;
  u32 hold;
  s16 ir_x;
  s16 ir_y;
} TestPad;

static void test_pad(BtInput *in, u32 frame, void *user) {
  const TestPad *t = (const TestPad *)user;
  in->buttons = t->period && frame % t->period < t->hold ? t->mask : 0u;
  in->ir_x = t->ir_x;
  in->ir_y = t->ir_y;
}

static GxCp *g_trace_cp;
static Chassis *g_trace_ch;

/* Samples where the guest actually is. A spin inside a single translated block
 * never reaches the dispatch counter, so block-count progress goes silent and
 * a hang looks identical to a very slow run. */
static void on_trace_alarm(int sig) {
  (void)sig;
  if (g_trace_ch) {
    fprintf(stderr,
            "sample pc=0x%08x last_pc=0x%08x mmio_r=%u mmio_w=%u hits=%u "
            "gxbytes=%llu gxcmds=%llu\n",
            g_trace_ch->cpu.pc, g_trace_ch->last_pc, g_trace_ch->mmio_reads,
            g_trace_ch->mmio_writes, g_trace_ch->dispatch_hits,
            (unsigned long long)gx_cp_bytes_in(g_trace_ch->gx),
            (unsigned long long)gx_cp_commands(g_trace_ch->gx));
    fflush(stderr);
  }
  alarm(5);
}
/* A hang is exactly the case the trace exists for, so flush it on the way out
 * rather than only on a clean finish. */
static void on_trace_signal(int sig) {
  (void)sig;
  gx_cp_trace_flush(g_trace_cp);
  _exit(1);
}

int main(int argc, char **argv) {
  const char *dol = argc > 1 ? argv[1] : "generated/extracted/identity/sys/main.dol";
  u32 cap = argc > 2 ? (u32)strtoul(argv[2], NULL, 10) : 50000u;

  void *mem1 = calloc(1, GC_MAIN_RAM_SIZE);
  void *mem2 = calloc(1, WII_MEM2_SIZE);
  if (!mem1 || !mem2) {
    fprintf(stderr, "host alloc failed\n");
    return 1;
  }

  static Chassis ch;
  if (!chassis_init(&ch, mem1, GC_MAIN_RAM_SIZE, mem2, WII_MEM2_SIZE, host_log,
                    NULL)) {
    fprintf(stderr, "chassis_init failed\n");
    return 1;
  }
  {
    const char *game = getenv("GV_GAME_ROOT");
    const char *nand = getenv("GV_NAND_ROOT");
    if (!game)
      game = "generated/extracted/game";
    if (!nand)
      nand = "generated/nand";
    if (!chassis_attach_media(&ch, game, nand))
      fprintf(stderr, "warning: media attach incomplete (game=%s nand=%s)\n",
              game, nand);
  }
  {
    const char *n = getenv("GV_BT_LOG");
    if (n)
      chassis_set_bt_log(&ch, (u32)strtoul(n, NULL, 10));
    const char *r = getenv("GV_BT_REPORTS");
    if (r)
      ch.ios.bt.log_reports = (u32)strtoul(r, NULL, 10);
  }
  {
    const char *from = getenv("GV_CENSUS_FROM");
    if (from)
      ch.census_from = (u32)strtoul(from, NULL, 10);
  }
  {
    /* GV_TRACE=lo:hi[:max] logs every block entry inside an address window --
     * the only practical way to watch one library's state machine run. */
    const char *tr = getenv("GV_TRACE");
    if (tr) {
      char *end = NULL;
      ch.trace_lo = (u32)strtoul(tr, &end, 16);
      ch.trace_hi = end && *end == ':' ? (u32)strtoul(end + 1, &end, 16) : 0u;
      ch.trace_left =
          end && *end == ':' ? (u32)strtoul(end + 1, NULL, 10) : 4000u;
    }
  }
  static TestPad pad;
  {
    const char *btn = getenv("GV_BTN");
    const char *ir = getenv("GV_IR");
    const char *per = getenv("GV_BTN_PERIOD");
    const char *hold = getenv("GV_BTN_HOLD");
    pad.mask = btn ? (u16)strtoul(btn, NULL, 16) : 0u;
    pad.period = per ? (u32)strtoul(per, NULL, 10) : 120u;
    pad.hold = hold ? (u32)strtoul(hold, NULL, 10) : 10u;
    pad.ir_x = -1;
    pad.ir_y = -1;
    if (ir) {
      char *end = NULL;
      pad.ir_x = (s16)strtol(ir, &end, 10);
      pad.ir_y = end && *end == ',' ? (s16)strtol(end + 1, NULL, 10) : 0;
    }
    if (btn || ir) {
      chassis_set_input_fn(&ch, test_pad, &pad);
      fprintf(stderr, "pad mask=0x%04x period=%u hold=%u ir=%d,%d\n", pad.mask,
              pad.period, pad.hold, pad.ir_x, pad.ir_y);
    }
  }
  /* Software renderer, off unless asked for: GV_GFX_DUMP names a directory to
   * write frames into, GV_GFX_FRAMES caps how many. */
  SoftGfx *gfx = NULL;
  const char *gfx_dir = getenv("GV_GFX_DUMP");
  if (gfx_dir) {
    const char *nf = getenv("GV_GFX_FRAMES");
    gfx = soft_gfx_create(gx_cp_regs(ch.gx), gfx_dir, nf ? atoi(nf) : 8);
    if (gfx) {
      GxSink sink;
      soft_gfx_sink(gfx, &sink);
      gx_cp_set_sink(ch.gx, &sink);
      printf("gfx software renderer on, frames -> %s\n", gfx_dir);
    }
  }
  {
    const char *tp = getenv("GV_GX_TRACE");
    if (tp) {
      gx_cp_trace(ch.gx, tp);
      g_trace_cp = ch.gx;
      g_trace_ch = &ch;
      signal(SIGALRM, on_trace_alarm);
      alarm(5);
      signal(SIGINT, on_trace_signal);
      signal(SIGTERM, on_trace_signal);
    }
  }
  if (!chassis_load_dol(&ch, dol))
    return 1;

  chassis_run(&ch, cap);
  printf("stop=%s hits=%u misses=%u fifo=%u ea=0x%08x pc=0x%08x "
         "mmio_r=%u mmio_w=%u smc=%u last=0x%08x retrace=%u gx=%u\n",
         chassis_stop_name(ch.stop_reason), ch.dispatch_hits, ch.dispatch_misses,
         ch.fifo_writes, ch.fifo_ea, ch.fifo_pc, ch.mmio_reads, ch.mmio_writes,
         ch.smc_hits, ch.last_pc, ch.flipper.retrace, (u32)gx_cp_commands(ch.gx));
  printf("lr=0x%08x ctr=0x%08x r1=0x%08x msr=0x%08x irq=%u cause=0x%x "
         "mask=0x%x\n",
         ch.last_lr, ch.last_ctr, ch.cpu.gpr[1], ch.cpu.msr, ch.irq_taken,
         ch.flipper.pi_cause, ch.flipper.pi_mask);
  printf("dec writes=%u asserts=%u last=0x%08x\n", ch.dec_writes,
         ch.dec_asserts, ch.dec);
  printf("cycles=%llu tb=%llu frames=%u\n", (unsigned long long)ch.cycles,
         (unsigned long long)ch.cpu.timebase, ch.flipper.retrace);
  /* The hand-written parser carried a large desync-diagnostic apparatus --
   * per-opcode census, raw byte windows, CP write history, unknown-opcode
   * context. All of it existed to find out where that parser was wrong.
   * Dolphin's decoder is now the reference, so the useful signal is just
   * whether anything failed to decode or failed to fetch. */
  printf("gx bytes=%llu cmds=%llu draws=%llu vtx=%llu unk=%llu pending=%u\n",
         (unsigned long long)gx_cp_bytes_in(ch.gx),
         (unsigned long long)gx_cp_commands(ch.gx),
         (unsigned long long)gx_cp_draws(ch.gx),
         (unsigned long long)gx_cp_vertices(ch.gx),
         (unsigned long long)gx_cp_unknown(ch.gx), gx_cp_pending(ch.gx));
  printf("gx dls=%llu dlbytes=%llu unresolved=%llu undecoded=%llu\n",
         (unsigned long long)gx_cp_display_lists(ch.gx),
         (unsigned long long)gx_cp_display_list_bytes(ch.gx),
         (unsigned long long)gx_cp_display_lists_unresolved(ch.gx),
         (unsigned long long)gx_cp_undecoded(ch.gx));
  if (ch.have_disc && ch.disc.seq) {
    /* The last files the game asked for, newest first: the boot's high-water
     * mark. Capped because a full run touches hundreds. */
    printf("disc files touched=%u, last 24 in load order:\n", ch.disc.seq);
    for (u32 want = ch.disc.seq; want + 24u > ch.disc.seq && want; want--)
      for (u32 i = 0; i < ch.disc.count; i++)
        if (ch.disc.entries[i].first_seen == want) {
          printf("  %4u %s\n", want, ch.disc.entries[i].path);
          break;
        }
  }
  if (gfx)
    soft_gfx_report(gfx);
  {
    const Bt *bt = &ch.ios.bt;
    printf("bt cmds=%u unknown=%u events=%u acl_in=%u acl_out=%u reports=%u "
           "conn=%d intr=%d mode=0x%02x cont=%d ext=%d ir=%d leds=0x%02x "
           "out_seen=0x%04x pend=%u\n",
           bt->hci_cmds, bt->hci_unknown, bt->events_out, bt->acl_in_n,
           bt->acl_out, bt->reports, bt->connected, bt->intr_up,
           bt->report_mode, bt->continuous, bt->ext_enabled, bt->ir_on,
           bt->leds, bt->out_seen, ch.ios.bt_read_n);
    printf("bt hci:");
    for (u32 i = 0; i < bt->cmd_log_n; i++)
      printf(" %04x", bt->cmd_log[i]);
    printf("\n");
  }
  {
    /* The SDK's 0x900 stub, as it actually sits in RAM: the branch it takes
     * decides whether a decrementer interrupt reaches OSAlarm or falls through
     * to __OSUnhandledException. */
    const char *v = getenv("GV_VEC_DUMP");
    if (v) {
      u32 base = (u32)strtoul(v, NULL, 16);
      for (u32 i = 0; i < 48; i++)
        printf("vec %08x  %08x\n", base + 4 * i,
               mem_read32(&ch.cpu, base + 4 * i));
    }
  }
  printf("ios opens=%u ioctls=%u ioctlvs=%u replies=%u hangs=%u last='%s'\n",
         ch.ios.opens, ch.ios.ioctls, ch.ios.ioctlvs, ch.ios.replies,
         ch.ios.hangs, ch.ios.last_path);
  printf("ios replyq head=%u tail=%u dropped=%u ctrl=0x%02x irqflag=0x%08x "
         "irqmask=0x%08x cause=0x%05x\n",
         ch.ios.rq_head, ch.ios.rq_tail, ch.ios.rq_dropped, ch.flipper.ipc_ctrl,
         ch.flipper.ipc_irq_flag, ch.flipper.ipc_irq_mask, ch.flipper.pi_cause);
  {
    static const char *names[] = {"none", "es",   "fs",   "di",  "stm_i",
                                  "stm_e", "oh1", "bt",   "net", "sdio",
                                  "usb",   "aes", "sha",  "other"};
    printf("ios dev census (open/ioctl/ioctlv):");
    for (int d = 0; d <= IOS_DEV_OTHER; d++) {
      if (!ch.ios.dev_opens[d] && !ch.ios.dev_ioctls[d] &&
          !ch.ios.dev_ioctlvs[d])
        continue;
      printf(" %s=%u/%u/%u", names[d], ch.ios.dev_opens[d],
             ch.ios.dev_ioctls[d], ch.ios.dev_ioctlvs[d]);
    }
    printf("  di_cmds=%u last_di=0x%02x\n", ch.ios.di_cmds, ch.ios.di_last_cmd);
  }
  {
    static u32 pcs[CHASSIS_HOT_SLOTS], hits[CHASSIS_HOT_SLOTS];
    u32 n = chassis_top_pcs(&ch, pcs, hits, CHASSIS_HOT_SLOTS);
    u32 show = n < 24u ? n : 24u;
    printf("hot pcs (%u distinct):\n", n);
    for (u32 i = 0; i < show; i++)
      printf("  0x%08x %u\n", pcs[i], hits[i]);
    const char *dump = getenv("GV_PC_CENSUS");
    if (dump) {
      FILE *f = fopen(dump, "w");
      if (f) {
        for (u32 i = 0; i < n; i++)
          fprintf(f, "%08x %u\n", pcs[i], hits[i]);
        fclose(f);
        printf("pc census -> %s\n", dump);
      }
    }
  }
  printf("disc reads=%u bytes=%llu misses=%u fst=0x%08x/0x%x\n",
         ch.disc.reads, (unsigned long long)ch.disc.bytes, ch.disc.misses,
         ch.fst_addr, ch.fst_size);
  printf("di reads=%u bytes=%u misses=%u | fs opens=%u misses=%u reads=%u "
         "bytes=%u\n",
         ch.ios.di_reads, ch.ios.di_read_bytes, ch.ios.di_misses,
         ch.ios.fs_opens, ch.ios.fs_misses, ch.ios.fs_reads,
         ch.ios.fs_read_bytes);
  printf("mmio unique=%u:", ch.flipper.unique_n);
  for (u32 i = 0; i < ch.flipper.unique_n && i < 32; i++)
    printf(" 0x%08x", ch.flipper.unique_ea[i]);
  printf("\n");

  int ok = ch.dispatch_hits > 0 && ch.stop_reason != CHASSIS_STOP_SMC;
  chassis_shutdown(&ch);
  free(mem1);
  free(mem2);
  return ok ? 0 : 1;
}

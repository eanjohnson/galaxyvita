#ifndef GALAXYVITA_CHASSIS_H
#define GALAXYVITA_CHASSIS_H

#include "cpu/cpu.h"
#include "disc.h"
#include "flipper.h"
#include "gx_cp.h"
#include "ios.h"
#include "nand.h"

enum {
  CHASSIS_STOP_RUNNING = 0,
  CHASSIS_STOP_FIFO = 1,
  CHASSIS_STOP_MISS = 2,
  CHASSIS_STOP_EXCEPTION = 3,
  CHASSIS_STOP_CAP = 4,
  CHASSIS_STOP_SMC = 5,
  CHASSIS_STOP_SPIN = 6
};

typedef void (*ChassisLogFn)(const char *msg, void *user);
/* Called at the remote's 200 Hz report rate, just before the report is built,
 * so a host can sample its own pad in step with the model. `frame` is the VI
 * retrace count, which is all a scripted test needs to time a button press. */
typedef void (*ChassisInputFn)(BtInput *in, u32 frame, void *user);

/* Block-entry PC census. Open-addressed, never resized: a boot that stalls
 * shows up as a handful of PCs with almost all the hits. */
enum {
  CHASSIS_PH_GUEST = 0,  /* inside recompiled code */
  CHASSIS_PH_IDLE,       /* recompiled code, but the SDK's idle wait */
  CHASSIS_PH_DISPATCH,   /* the chassis loop's own per-block work */
  CHASSIS_PH_GX,         /* FIFO command parse, the three below split out */
  CHASSIS_PH_IOS,        /* IOS/Bluetooth service, flipper, interrupts */
  CHASSIS_PH_MMIO,       /* other emulated MMIO */
  CHASSIS_PH_DISC,       /* blocked on the memory card, inside an IOS read */
  /* What used to be one gx phase. Split because the nogfx A/B showed the
   * halves differ by 2.7x at the slow end and rank different work: GXVTX is
   * the vertex loader and the per-draw vectors it fills, GXSUB is sceGxmDraw
   * and the state re-issued around it, GXCOPY is the copy and present. */
  CHASSIS_PH_GXVTX,
  CHASSIS_PH_GXSUB,
  CHASSIS_PH_GXCOPY,
  CHASSIS_PH_COUNT
};

#define CHASSIS_HOT_SLOTS 4096u

typedef struct Chassis {
  CPUState cpu;
  Flipper flipper;
  GxCp *gx;
  Ios ios;
  Disc disc;
  Nand nand;
  int have_disc;
  int have_nand;
  u32 fst_addr;
  u32 fst_size;
  u32 dvd_log_left;
  /* Remaining Bluetooth-stack trace lines to print. */
  u32 bte_log_left;
  /* Block-entry trace window: every dispatch into [trace_lo, trace_hi) is
   * logged until trace_left runs out. Off unless a caller sets a range. */
  u32 trace_lo;
  u32 trace_hi;
  u32 trace_left;
  /* Block number the window opens at, so a trace can start past the boot. */
  u32 trace_from;
  u32 hle_calls;
  /* CPU cycles not yet credited to the timebase. */
  u32 tb_frac;
  /* CPU cycles not yet credited to the Wii Remote's 200 Hz report clock. */
  u32 bt_frac;
  u64 cycles;
  ChassisLogFn log;
  void *log_user;
  ChassisInputFn input_fn;
  void *input_user;
  /* Block count after which the PC census is cleared and restarted; 0 keeps
   * the whole run. */
  u32 census_from;
  u32 dispatch_hits;
  u32 dispatch_misses;
  u32 mmio_reads;
  u32 mmio_writes;
  u32 fifo_writes;
  u32 fifo_ea;
  u32 fifo_pc;
  u32 last_pc;
  /* Which part of the emulator is running right now, for a sampling profiler
   * to read from another thread. One byte store at a handful of sites, no
   * timers on the hot path: the sampler turns a stream of these into a time
   * breakdown, which is the thing this project has never had on the console.
   * CHASSIS_PH_IDLE is deliberately split out of GUEST -- it is guest code,
   * but it is guest code doing nothing, and how much of the run it owns is
   * the whole case for idle-skip. */
  volatile u8 phase;
  u32 last_lr;
  u32 last_ctr;
  u32 ran_from;
  u32 spin_gpr[32];
  u32 uncovered_pc;
  u32 smc_hits;
  u32 spin_count;
  u32 exception;
  u32 dec;
  u32 hid4;
  u32 spr_skips;
  /* How often the recompiled code hands an instruction back to the chassis
   * interpreter, by class. Nothing counted this before, and the one time it
   * mattered -- DCFlushRange falling through per cache line -- it was found by
   * accident in a hot-PC list. mftb is on the guest's own timing paths, so a
   * large count here is a real cost and not just a curiosity. */
  /* Stop once the emulated VI has retraced this many times; 0 is off.
   *
   * A block cap counts *host dispatches*, which is the wrong ruler for any
   * change that alters how much guest code one dispatch covers -- cross-chunk
   * direct calls and idle skipping both do, so under a block cap they run the
   * game further and every downstream counter moves for reasons that have
   * nothing to do with correctness. Retraces are the guest's own clock, so
   * stopping on one compares two builds at the same point in the game and
   * FIFO bytes and GX commands then have to match exactly. */
  u32 retrace_cap;
  u32 fb_mfspr;
  u32 fb_mtspr;
  u32 fb_mftb;
  u32 fb_unknown;
  u32 dec_writes;
  u32 dec_asserts;
  u32 irq_taken;
  u32 sprg[4];
  int dec_irq;
  int idle_note;
  u32 idle_at;
  int fifo_hit;
  int stop_reason;
  int owns_ram;
  int stop_on_fifo;
  u32 hot_pc[CHASSIS_HOT_SLOTS];
  u32 hot_hits[CHASSIS_HOT_SLOTS];
  /* WUDIsLinkedWBC instrumentation -- see hle_wud_is_ready in chassis.c. */
  u32 wud_log;
  int wud_real;
} Chassis;

int chassis_init(Chassis *ch, void *mem1, u32 mem1_size, void *mem2,
                 u32 mem2_size, ChassisLogFn log, void *log_user);
/* Attach the extracted game tree and the generated NAND tree. Either path may
 * be NULL. Must be called before chassis_load_dol so the apploader HLE can
 * place the FST and setting.txt. Returns 1 if both requested trees opened. */
int chassis_attach_media(Chassis *ch, const char *game_root,
                         const char *nand_root);
int chassis_load_dol(Chassis *ch, const char *path);
int chassis_run(Chassis *ch, u32 max_blocks);
void chassis_shutdown(Chassis *ch);
const char *chassis_stop_name(int reason);
/* Fill out_pc/out_hits with the `n` hottest block-entry PCs, descending.
 * Returns how many were written. */
u32 chassis_top_pcs(const Chassis *ch, u32 *out_pc, u32 *out_hits, u32 n);
/* Hand the Wii Remote model the player's current state. The next 200 Hz report
 * carries it, so a host can call this whenever its own pad is sampled. */
void chassis_set_input(Chassis *ch, const BtInput *in);
/* Sample the host's pad from inside the run loop instead. Either may be used;
 * the callback wins, because it runs last. */
void chassis_set_input_fn(Chassis *ch, ChassisInputFn fn, void *user);
/* Log the next `packets` HCI commands and ACL packets in both directions. */
void chassis_set_bt_log(Chassis *ch, u32 packets);

#endif

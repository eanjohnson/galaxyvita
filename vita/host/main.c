/* GalaxyVita CoreHost Phase 4: AOT + Flipper HLE + the sceGxm backend. */
#include "chassis.h"
#include "gx_gxm.h"
#include "gxm.h"
#include "shader_cache.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define AFFINITY_CPU0 0x10000
#define AFFINITY_CPU1 0x20000
#define AFFINITY_CPU2 0x40000

#define LOG_PATH "ux0:data/galaxyvita/boot.log"
#define WATCH_PATH "ux0:data/galaxyvita/watchdog.log"
#define PROBE_PATH "ux0:data/galaxyvita/watchdog.log.probe"
#define DOL_PATH "ux0:data/galaxyvita/sys/main.dol"
/* The disc and NAND trees the user's own extraction produces. Neither ships in
 * the VPK; see scripts/extract-disc.sh. */
#define GAME_ROOT "ux0:data/galaxyvita/game"
#define NAND_ROOT "ux0:data/galaxyvita/nand"
/* Compiled shaders, kept between runs. Not in the VPK: they are produced by
 * the console's own compiler on first run. */
#define GXP_DIR "ux0:data/galaxyvita/gxp"
/* Presented frames, pulled off the console and diffed against the host
 * rasteriser's PNGs of the same boot. */
#define FRAME_DIR "ux0:data/galaxyvita/frames"
/* Touch this file on the console to boot with graphics off. There are no
 * environment variables to switch behaviour with here, and the question
 * "is the new backend what stopped the boot?" has to be answerable without
 * building a second binary. */
#define NOGFX_FLAG "ux0:data/galaxyvita/nogfx.txt"
#define IDLE_FLAG "ux0:data/galaxyvita/idle.txt"
/* Buttons to hold down without anyone holding them. The boot is gated by
 * screens that want a button -- the wrist strap wants A, the title screen wants
 * A and B -- and neither vitacompanion's `press` nor anything else on the host
 * can reach sceCtrlPeekBufferPositive, so on the console those screens would
 * need a person at the device. That is the one thing device iteration here is
 * not allowed to need.
 *
 * The file holds a hex mask in the remote's own button order (A is 0x0800, B is
 * 0x0400), optionally followed by a period and a hold in frames: "0c00 90 30"
 * holds A and B for 30 frames out of every 90. A button held forever is not the
 * same input as a button pressed, and menus that advance on the edge never see
 * one, which is why the duty cycle is there. Empty or absent means the pad
 * alone drives the game, as it should when someone is actually playing. */
#define BTN_FLAG "ux0:data/galaxyvita/btn.txt"
#define INPUT_FLAG "ux0:data/galaxyvita/input.txt"
#define SNAP_FLAG "ux0:data/galaxyvita/snap.txt"
/* Blocks, not frames. 60M stopped inside the boot: on the host the title screen
 * is around 90M and the file select around 120M, and the console runs the same
 * blocks in the same order. Overshooting costs nothing -- the run ends when the
 * boot does -- while undershooting looks exactly like a hang.
 *
 * blocks.txt overrides it, as a decimal count, so the cap can be moved without
 * resending a 22 MB eboot. */
#define BLOCK_CAP 250000000u
#define BLOCKS_FLAG "ux0:data/galaxyvita/blocks.txt"

static u32 read_block_cap(void) {
  SceUID fd = sceIoOpen(BLOCKS_FLAG, SCE_O_RDONLY, 0);
  if (fd < 0)
    return BLOCK_CAP;
  char buf[32];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return BLOCK_CAP;
  buf[n] = 0;
  unsigned long v = strtoul(buf, NULL, 10);
  return v ? (u32)v : BLOCK_CAP;
}

/* How many frames to dump and how far apart, from a flag file.
 *
 * The defaults covered frames 0 to 112, which at ~45k blocks per frame is the
 * first three seconds of guest time -- and the first white screens pulled off
 * the card turned out to be that opening flash, which the host renders white
 * too. Every dump was of a moment nobody had a question about. The interesting
 * frames are thousands in, so the stride has to be settable without a rebuild
 * and another 35 MB upload. Format: "<count> <stride>". */
/* Which fragment shader the GXM backend's textured path uses; see
 * gx_gxm_set_frag_mode(). A flag file rather than a rebuild because each device
 * cycle is a 35 MB upload and minutes of guest time before the frame that
 * answers the question. */
#define FRAGDBG_FLAG "ux0:data/galaxyvita/fragdbg.txt"

static unsigned read_fragdbg(void) {
  SceUID fd = sceIoOpen(FRAGDBG_FLAG, SCE_O_RDONLY, 0);
  if (fd < 0)
    return 0u;
  char buf[16];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return 0u;
  buf[n] = 0;
  return (unsigned)strtoul(buf, NULL, 10);
}

#define DUMP_FLAG "ux0:data/galaxyvita/dump.txt"
static unsigned g_dump_count = 16u, g_dump_stride = 256u;

static void read_dump_flag(void) {
  SceUID fd = sceIoOpen(DUMP_FLAG, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  char buf[64];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  unsigned c = 0, st = 0;
  if (sscanf(buf, "%u %u", &c, &st) == 2 && c && st) {
    g_dump_count = c;
    g_dump_stride = st;
  }
}

/* Guest RAM is memblocks, not heap, so the heap exists almost entirely for
 * ShaccCg: it churns tens of megabytes per compile and how many compiles
 * survive one process scales with how much heap there is. The shader cache
 * persists its results, so this is paid on first run and on new shaders only. */
unsigned int _newlib_heap_size_user = 128u * 1024u * 1024u;

static int g_gfx_ready = 0;
/* What the heartbeat is actually doing, reported where it can be seen. The
 * probe proved that presenting keeps an app alive; this app presents and dies
 * anyway, so the question is whether its presents are happening at all. */
static volatile unsigned g_presents;
static volatile int g_present_rc = 1;

static SceUID g_log_mtx = -1;
static SceUID g_mem1_uid = -1;
static SceUID g_mem2_uid = -1;
static void *g_mem1 = NULL;
static void *g_mem2 = NULL;
static volatile int g_cpu_done = 0;

static void ensure_data_dir(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/galaxyvita", 0777);
  sceIoMkdir("ux0:data/galaxyvita/sys", 0777);
  sceIoMkdir("ux0:data/galaxyvita/game", 0777);
  sceIoMkdir("ux0:data/galaxyvita/nand", 0777);
}

/* Buffered, because the unbuffered version was throttling the emulator.
 *
 * One open/append/close per line on a memory card costs tens of milliseconds,
 * and the boot logs a line per MMIO write to the PPC control register. The
 * dispatch rate decayed 9600 -> 9400 -> 3300 -> 1200 blocks per two seconds as
 * the log grew, and then the console looked hung: it was not, it was queueing
 * behind its own logging, and the watchdog's own writes were starved behind it
 * too. Batching to one write per 12 KB or per watchdog tick is ~100x fewer
 * file operations for the same text.
 *
 * Still open/append/close per flush rather than a held handle, so the file's
 * size is committed and the host can read the log over FTP while the boot
 * runs -- which is the only way to watch it.
 *
 * The per-line sceClibPrintf is gone with it. Nothing on this console reads
 * that sink unless a logging plugin is installed, and it was one more
 * synchronous call on the same hot path. */
static char g_log_buf[16384];
static u32 g_log_used;

static void log_flush_locked(void) {
  if (!g_log_used)
    return;
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, g_log_buf, g_log_used);
    sceIoClose(fd);
  }
  g_log_used = 0;
}

static void log_flush(void) {
  if (g_log_mtx >= 0)
    sceKernelLockMutex(g_log_mtx, 1, NULL);
  log_flush_locked();
  if (g_log_mtx >= 0)
    sceKernelUnlockMutex(g_log_mtx, 1);
}

/* Flush only if the lock is free. The watchdog must never block on the thread
 * it is watching: if the emulator is wedged inside log_flush_locked() holding
 * the mutex, a blocking flush here takes the watchdog down with it and the
 * symptom becomes "no watchdog output", which is indistinguishable from a dead
 * process. Returns 0 when the lock was busy, which is itself a diagnosis. */
static int log_try_flush(void) {
  if (g_log_mtx < 0)
    return 1;
  if (sceKernelTryLockMutex(g_log_mtx, 1) < 0)
    return 0;
  log_flush_locked();
  sceKernelUnlockMutex(g_log_mtx, 1);
  return 1;
}

static void log_line(const char *msg) {
  size_t n = strlen(msg);
  if (g_log_mtx >= 0)
    sceKernelLockMutex(g_log_mtx, 1, NULL);
  if (n + 1u > sizeof(g_log_buf) - g_log_used)
    log_flush_locked();
  if (n + 1u <= sizeof(g_log_buf)) {
    memcpy(g_log_buf + g_log_used, msg, n);
    g_log_used += (u32)n;
    g_log_buf[g_log_used++] = '\n';
  }
  if (g_log_used > sizeof(g_log_buf) - 4096u)
    log_flush_locked();
  if (g_log_mtx >= 0)
    sceKernelUnlockMutex(g_log_mtx, 1);
}

static void chassis_log(const char *msg, void *user) {
  (void)user;
  log_line(msg);
}

/* The Vita's pad, presented to the game as a Wii Remote with a nunchuk in it.
 * SMG reads the pointer, A and B, the nunchuk stick and its Z and C; the rest
 * is mapped where it falls naturally so menus and the HOME button work. The
 * right stick drives the pointer as an absolute position, which is as close as
 * a stick gets to aiming a remote at the screen. */
static u8 stick_axis(unsigned char v, int invert) {
  int d = (int)v - 128;
  if (invert)
    d = -d;
  /* 0x20..0xE0 is the nunchuk's own travel, 0x80 centred. */
  int out = 0x80 + d * 0x60 / 0x80;
  if (out < 0x20)
    out = 0x20;
  if (out > 0xE0)
    out = 0xE0;
  return (u8)out;
}

static s16 pointer_axis(unsigned char v, int centre, int half) {
  int out = centre + ((int)v - 128) * half / 128;
  if (out < 0)
    out = 0;
  if (out > centre * 2 - 1)
    out = centre * 2 - 1;
  return (s16)out;
}

/* Re-read from BTN_FLAG while the game runs; see the comment there. */
static u16 g_btn_mask;
static u32 g_btn_period = 90u;
static u32 g_btn_hold = 30u;

static void read_btn_flag(void) {
  const u16 was_mask = g_btn_mask;
  const u32 was_period = g_btn_period, was_hold = g_btn_hold;
  SceUID fd = sceIoOpen(BTN_FLAG, SCE_O_RDONLY, 0);
  if (fd < 0)
    return;
  char buf[64];
  int n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  unsigned mask = 0, period = 0, hold = 0;
  int got = sscanf(buf, "%x %u %u", &mask, &period, &hold);
  if (got < 1)
    return;
  g_btn_mask = (u16)mask;
  if (got >= 2 && period)
    g_btn_period = period;
  if (got >= 3)
    g_btn_hold = hold;
  if (g_btn_hold > g_btn_period)
    g_btn_hold = g_btn_period;
  /* Logged only when it changes. This is polled every two seconds now, and a
   * line a tick would bury the boot log it shares. */
  if (g_btn_mask != was_mask || g_btn_period != was_period ||
      g_btn_hold != was_hold) {
    char line[96];
    snprintf(line, sizeof(line), "btn flag: mask=0x%04x period=%u hold=%u",
             (unsigned)g_btn_mask, (unsigned)g_btn_period, (unsigned)g_btn_hold);
    log_line(line);
  }
}

/* A scripted remote, re-read from the card while the game runs.
 *
 * btn.txt presses one mask forever on a duty cycle, which is enough to get
 * past a screen that only wants any button and no use at all for anything
 * that needs a particular sequence. This is the other half: input.txt holds a
 * list of (mask, frames) steps played once, in order, and the file is polled
 * by the watchdog thread rather than parsed at startup -- so a run already in
 * progress can be driven from the host without restarting it. Nothing on this
 * console can deliver synthetic input any other way: vitacompanion's `press`
 * reaches neither SceShell nor the application, and vita_pad reads the same
 * sceCtrlPeekBufferPositive that it fails to reach.
 *
 * The first token is a serial. The script is (re)started whenever it changes,
 * which is what lets the same press be sent twice -- comparing the steps
 * themselves would swallow the second one. */
enum { INPUT_STEPS = 64 };

typedef struct {
  u16 mask; /* Wii Remote buttons, BT_BTN_* */
  u8 nun;   /* nunchuk buttons, BT_NUN_* */
  u32 frames;
} InputStep;

/* Two buffers: the watchdog thread fills the spare one and then publishes it
 * by bumping the generation, so a half-parsed script is never the one being
 * played. The players's worst case is reading one step from the wrong buffer
 * for a single 200 Hz sample, which is a frame of input, not a wedge. */
static InputStep g_script[2][INPUT_STEPS];
static volatile u32 g_script_n[2];
static volatile u32 g_script_slot;
static volatile u32 g_script_gen;    /* bumped when a new script is published */
static volatile s32 g_script_ir[2];  /* pointer park, negative to leave alone */
static u32 g_script_serial;          /* last serial applied */
static u32 g_script_gen_seen;        /* the player's copy of the generation */
static u32 g_script_start;           /* retrace the current script began at */

static void read_input_flag(void) {
  SceUID fd = sceIoOpen(INPUT_FLAG, SCE_O_RDONLY, 0);
  char buf[512];
  int n;
  unsigned serial = 0;
  int ir_x = -1, ir_y = -1;
  const char *p;
  char *end;
  unsigned slot, count = 0;

  if (fd < 0)
    return;
  n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  p = buf;
  serial = (unsigned)strtoul(p, &end, 10);
  if (end == p)
    return;
  p = end;
  /* The pointer park, in the IR camera's own 1024x768 units, and negative to
   * leave the pad's own pointer alone. Always present, never optional: when
   * it was optional it was read as "the next two numbers in the file", which
   * is the first step, and the whole script parsed as empty. A header that is
   * always three numbers cannot do that. */
  {
    const char *q = p;
    long x = strtol(q, &end, 10);
    if (end == q)
      return;
    q = end;
    ir_y = (int)strtol(q, &end, 10);
    if (end == q)
      return;
    ir_x = (int)x;
    p = end;
  }
  if (serial == g_script_serial)
    return;

  slot = g_script_slot ? 0u : 1u;
  for (;;) {
    unsigned long mask, frames;
    const char *q = p;
    mask = strtoul(q, &end, 16);
    if (end == q)
      break;
    q = end;
    frames = strtoul(q, &end, 10);
    if (end == q)
      break;
    p = end;
    if (count >= INPUT_STEPS || frames == 0ul)
      break;
    /* The nunchuk rides in bits 16 and up of the same field, so a step is
     * always exactly two numbers and the parse cannot go out of step. */
    g_script[slot][count].mask = (u16)(mask & 0xFFFFul);
    g_script[slot][count].nun = (u8)((mask >> 16) & 0xFFul);
    g_script[slot][count].frames = (u32)frames;
    count++;
  }
  g_script_n[slot] = count;
  g_script_ir[0] = ir_x;
  g_script_ir[1] = ir_y;
  g_script_slot = slot;
  g_script_serial = serial;
  g_script_gen++;
  {
    char line[96];
    snprintf(line, sizeof(line), "input script: serial=%u steps=%u ir=%d,%d",
             serial, count, ir_x, ir_y);
    log_line(line);
  }
}

/* A frame on demand, so that a press can be seen to have done something. The
 * serial is passed straight through; the renderer takes one frame per change
 * and ignores a repeat. */
static void read_snap_flag(void) {
  SceUID fd = sceIoOpen(SNAP_FLAG, SCE_O_RDONLY, 0);
  char buf[32];
  int n;
  if (fd < 0)
    return;
  n = sceIoRead(fd, buf, sizeof(buf) - 1);
  sceIoClose(fd);
  if (n <= 0)
    return;
  buf[n] = 0;
  if (g_gfx_ready)
    gx_gxm_snap((unsigned)strtoul(buf, NULL, 10));
}

static void vita_pad(BtInput *in, u32 frame, void *user) {
  (void)user;
  SceCtrlData pad;
  if (sceCtrlPeekBufferPositive(0, &pad, 1) < 1)
    return;
  u16 b = 0;
  if (pad.buttons & SCE_CTRL_CROSS)
    b |= BT_BTN_A;
  if (pad.buttons & (SCE_CTRL_SQUARE | SCE_CTRL_RTRIGGER))
    b |= BT_BTN_B;
  if (pad.buttons & SCE_CTRL_UP)
    b |= BT_BTN_UP;
  if (pad.buttons & SCE_CTRL_DOWN)
    b |= BT_BTN_DOWN;
  if (pad.buttons & SCE_CTRL_LEFT)
    b |= BT_BTN_LEFT;
  if (pad.buttons & SCE_CTRL_RIGHT)
    b |= BT_BTN_RIGHT;
  if (pad.buttons & SCE_CTRL_START)
    b |= BT_BTN_PLUS;
  if (pad.buttons & SCE_CTRL_SELECT)
    b |= BT_BTN_MINUS;
  /* OR'd, not substituted: a real pad still works while the flag is set, so a
   * run can be taken over by hand without rebooting it. */
  if (g_btn_mask && (frame % g_btn_period) < g_btn_hold)
    b |= g_btn_mask;
  u8 nun = 0;
  {
    const u32 gen = g_script_gen;
    if (gen != g_script_gen_seen) {
      g_script_gen_seen = gen;
      g_script_start = frame;
    }
    {
      const u32 slot = g_script_slot;
      const u32 steps = g_script_n[slot];
      u32 t = frame - g_script_start;
      u32 i;
      for (i = 0; i < steps; i++) {
        if (t < g_script[slot][i].frames) {
          b |= g_script[slot][i].mask;
          nun |= g_script[slot][i].nun;
          break;
        }
        t -= g_script[slot][i].frames;
      }
    }
  }
  in->buttons = b;
  in->nunchuk = nun;
  if (pad.buttons & SCE_CTRL_LTRIGGER)
    in->nunchuk |= BT_NUN_Z;
  if (pad.buttons & SCE_CTRL_TRIANGLE)
    in->nunchuk |= BT_NUN_C;
  in->stick_x = stick_axis(pad.lx, 0);
  in->stick_y = stick_axis(pad.ly, 1);
  in->ir_x = pointer_axis(pad.rx, 512, 512);
  in->ir_y = pointer_axis(pad.ry, 384, 384);
  if (g_script_ir[0] >= 0) {
    in->ir_x = (s16)g_script_ir[0];
    in->ir_y = (s16)g_script_ir[1];
  }
  /* Mirror x once, after both producers, because bt.c's fill_ir puts the two
   * sensor-bar LEDs at this coordinate in *camera* units and the camera faces
   * the player: its image is left-right flipped against the screen, and the
   * game un-flips it. Feeding screen-space x into a camera-space field meant
   * the pointer ran backwards -- the right stick moved the cursor the wrong
   * way, and so did every scripted --ir. Measured on the file-select screen:
   * ir_x 128 put the cursor at screen x=875 and ir_x 896 put it at x=94, a
   * slope of -1.017 px per unit, which is this mirror and nothing else.
   *
   * Only here, so the sentinel survives: bt.c reads a negative ir_x as "the
   * camera sees nothing", and 1023 - (-1) would read as a pointer at the edge.
   * Note tests/host/aot_chassis.c fills the same field from GV_IR and is still
   * camera-space, so the two agree only at the centre. */
  if (in->ir_x >= 0)
    in->ir_x = (s16)(1023 - in->ir_x);
}

static void *vita_rw(const char *name, unsigned size, SceUID *uid) {
  SceKernelAllocMemBlockOpt opt;
  memset(&opt, 0, sizeof(opt));
  opt.size = sizeof(opt);
  *uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, &opt);
  if (*uid < 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "alloc %s failed: 0x%08x", name, (unsigned)*uid);
    log_line(buf);
    return NULL;
  }
  void *base = NULL;
  if (sceKernelGetMemBlockBase(*uid, &base) < 0)
    return NULL;
  {
    char buf[96];
    snprintf(buf, sizeof(buf), "alloc %s ok size=%u base=%p", name, size, base);
    log_line(buf);
  }
  return base;
}

static int pin_self(int affinity, const char *name) {
  int rc = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), affinity);
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
  char buf[192];
  snprintf(buf, sizeof(buf),
           "thread=%s tid=0x%08x affinity=0x%x currentCpu=%d lastCpu=%d rc=%d",
           name, (unsigned)sceKernelGetThreadId(),
           (unsigned)info.currentCpuAffinityMask, info.currentCpuId,
           info.lastExecutedCpuId, rc);
  log_line(buf);
  return rc;
}

/* There is no separate GPU thread any more. The GX sink is called from inside
 * the emulated CPU's FIFO writes, so the scene has to be recorded on that
 * thread; handing the draws to another one would only add a queue and a
 * lifetime problem to a path that is nowhere near the frame budget. The
 * display queue Sony starts inside sceGxmInitialize still does the swap and
 * the vblank wait on its own thread. */

/* The device stops logging a few seconds into the boot while the host, running
 * the same blocks, carries on for tens of millions. Three explanations fit
 * that evidence -- the emulated CPU is stuck, it is merely slow, or it is fine
 * and the logging from that thread has stopped working -- and they are
 * distinguished by a heartbeat from a *different* thread: no line at all means
 * the process or its file I/O is gone, a line with a frozen hit count means
 * the CPU thread is stuck and says where, and a line with a rising count means
 * only the logging was lost. */
static const Chassis *volatile g_watch;

static int watchdog(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  u32 last = 0;
  u32 ticks = 0;
  int log_stuck = 0;
  /* Opened once and held.
   *
   * The previous version opened the file per tick, and it stopped reporting
   * before the emulated CPU stopped making progress -- which is the signature
   * of sceIoOpen starting to fail, not of the boot dying. A handle taken at
   * the start keeps working across that, so if the boot really is fine and the
   * logging is what broke, this is what says so. The probe below measures it
   * directly: a fresh open every tick, reported through the held handle. */
  SceUID fd = sceIoOpen(WATCH_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                        0777);
  int wrote = 0;
  int probe = 0;
  u32 lost = 0;
  while (!g_cpu_done) {
    /* The CB form, so this thread is in a state where system callbacks can
     * actually be delivered. Nothing in this app was: main blocks in
     * sceKernelWaitThreadEnd and every worker used the plain delay, so no
     * thread could ever service a system event. */
    sceKernelDelayThreadCB(2 * 1000 * 1000);
    const Chassis *ch = g_watch;
    char buf[256];
    int n;
    if (!ch) {
      ticks++;
      continue;
    }
    /* This line goes out first, through this thread's own handle, before
     * anything that could block. Whatever else is stuck, the tick is reported. */
    u32 hits = ch->dispatch_hits;
    /* Free memory, every tick. The boot dies with every thread stopping at
     * once and the process still resident, which is what running out looks
     * like from the outside -- and the console is the only place it can
     * happen, since the host plateaus at 154 MB with room to spare. A trend
     * towards zero here says so; a flat line rules it out. */
    SceKernelFreeMemorySizeInfo mem;
    memset(&mem, 0, sizeof(mem));
    mem.size = sizeof(mem);
    if (sceKernelGetFreeMemorySize(&mem) < 0)
      mem.size_user = mem.size_cdram = mem.size_phycont = -1;
    /* A fresh open every tick, reported through the held handle. The held
     * handle exists because per-tick opens once started failing partway into
     * the boot; this says whether that is happening again, and it is the
     * difference between "the emulator stopped" and "the card stopped taking
     * writes", which look identical from the host. */
    SceUID pfd = sceIoOpen(PROBE_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                           0777);
    probe = pfd < 0 ? pfd : 0;
    if (pfd >= 0)
      sceIoClose(pfd);
    n = snprintf(buf, sizeof(buf),
                 "watchdog t=%us hits=%u (+%u) pc=0x%08x fifo=%u mmio_w=%u "
                 "free=%dk cdram=%dk phy=%dk wr=%d probe=%d lost=%u "
                 "io=%ums/%u "
                 "pres=%u prc=0x%08x%s%s\n",
                 ticks * 2u, hits, hits - last, ch->last_pc, ch->fifo_writes,
                 ch->mmio_writes, mem.size_user / 1024, mem.size_cdram / 1024,
                 mem.size_phycont / 1024, wrote, probe, lost,
                 (unsigned)(g_disc_io_us / 1000u), g_disc_io_n,
                 g_presents, (unsigned)g_present_rc,
                 hits == last ? "  STALLED" : "",
                 log_stuck ? "  LOGLOCKED" : "");
    wrote = sceIoWrite(fd, buf, (SceSize)(n > 0 ? n : 0));
    /* What the GXM backend has actually drawn, every tick.
     *
     * gx_gxm_report() existed from the day the backend did and had no caller
     * but the shutdown path -- so it printed only for a run that ended
     * cleanly, and no long device run ever has. The first console frames were
     * diagnosed by pulling raw framebuffers off the card instead, which works
     * and costs a round trip per frame. This is the same answer for free, and
     * it lands in the one file that keeps being written when the app's own
     * logging has given up. */
    if (g_gfx_ready) {
      char gbuf[352];
      int gn;
      gx_gxm_report(gbuf, sizeof(gbuf) - 2u);
      gn = (int)strlen(gbuf);
      gbuf[gn++] = '\n';
      sceIoWrite(fd, gbuf, (SceSize)gn);
    }
    /* A held handle buffers: without this the last ticks before a wedge sit in
     * the cache and never reach the card, which reads from the host exactly
     * like a thread that stopped running. The whole point of this thread is to
     * be believed about that distinction, so pay for the sync. */
    /* Which interrupt is firing, not just how many. A guest dispatching flat
     * out with a frozen GX FIFO is either starved of an interrupt or drowning
     * in one, and the edge counts per source are what tell the two apart.
     * Every tick, because the rate is the whole point. */
    {
      char ibuf[384];
      int in;
      in = snprintf(ibuf, sizeof(ibuf) - 2u,
                    "irq ai=%u dsp=%u vi=%u petok=%u pefin=%u ipc=%u "
                    "cause=0x%08x mask=0x%08x stuck=%u/%u cmd=%u fd=%u "
                    "ret=%d\n",
                    ch->flipper.irq_count[0], ch->flipper.irq_count[1],
                    ch->flipper.irq_count[2], ch->flipper.irq_count[3],
                    ch->flipper.irq_count[4], ch->flipper.irq_count[5],
                    ch->flipper.pi_cause, ch->flipper.pi_mask,
                    ch->ios.stuck_pumps, ch->ios.stuck_worst,
                    ch->ios.stuck_cmd, ch->ios.stuck_fd,
                    (int)ch->ios.stuck_ret);
      if (in > 0)
        in += snprintf(ibuf + in, sizeof(ibuf) - (size_t)in - 2u,
                       "disc reads=%u miss=%u mb=%u off=0x%llx len=%u\n",
                       ch->disc.reads, ch->disc.misses,
                       (unsigned)(ch->disc.bytes >> 20),
                       (unsigned long long)ch->disc.last_off,
                       ch->disc.last_len);
      if (in > 0)
        sceIoWrite(fd, ibuf, (SceSize)in);
    }
    /* The remote, polled rather than parsed at startup. This thread is
     * already the one doing card I/O on a timer, and two small reads a tick
     * are nothing beside the log it just wrote. */
    read_btn_flag();
    read_input_flag();
    read_snap_flag();
    sceIoSyncByFd(fd, 0);
    /* Never exit. The previous version returned when a reopen failed, which
     * made the one thread whose job is to say what happened the first thread
     * to go quiet -- and "no watchdog output" is indistinguishable from a dead
     * process, so a recoverable I/O error read as a hung boot. Count the lost
     * ticks instead and keep trying; the next line that lands reports how many
     * went missing and what the errors were. */
    if (wrote < 0 || fd < 0) {
      lost++;
      if (fd >= 0)
        sceIoClose(fd);
      fd = sceIoOpen(WATCH_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
    /* The emulator only flushes when its buffer fills, which during a quiet
     * stretch can be minutes. This keeps the log at most one tick stale -- but
     * never at the cost of this thread. */
    log_stuck = !log_try_flush();
    last = hits;
    ticks++;
  }
  if (fd >= 0)
    sceIoClose(fd);
  return 0;
}

/* The system stops an application that never puts anything on screen.
 *
 * That is what the "stall" was. Every boot died 10-12 seconds in with every
 * thread going quiet at once, the process still resident, memory flat and no
 * core dump -- and it did exactly the same with the emulator doing nothing at
 * all, which is what finally placed the cause outside the chassis. The guest
 * does not reach its first XFB copy for a long time yet, so nothing was ever
 * presented and the app was killed before the boot could get anywhere.
 *
 * This holds a black frame up until the GX backend starts presenting real
 * ones, and then gets out of the way -- sceGxm drives the display itself
 * through its own queue, and two writers would fight over it. */
static int display_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  pin_self(AFFINITY_CPU1, "display");
  SceUID uid = sceKernelAllocMemBlock("gv-heartbeat-fb",
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                      2 * 1024 * 1024, NULL);
  void *fb = NULL;
  if (uid >= 0 && sceKernelGetMemBlockBase(uid, &fb) < 0)
    fb = NULL;
  if (!fb) {
    log_line("display heartbeat: no framebuffer; the app may be stopped early");
    return 0;
  }
  memset(fb, 0, 2 * 1024 * 1024);
  log_line("display heartbeat up");
  while (!g_cpu_done) {
    /* Keep the console awake from inside the app, every iteration of the only
     * thread that runs for the whole life of the run.
     *
     * Auto-standby was killing bubble launches at ~3 minutes, and the run that
     * reached 708 s only did so because a host-side loop was re-asserting
     * `nosleep on` over vitacompanion every 30 s -- which is exactly the
     * someone-at-the-device step this project is not allowed to need. DEFAULT
     * cancels the suspend, OLED-off and dimming timers together, so the screen
     * also stops dimming while a long run is being watched. */
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
    if (!g_gfx_ready) {
      SceDisplayFrameBuf f;
      memset(&f, 0, sizeof(f));
      f.size = sizeof(f);
      f.base = fb;
      f.pitch = 960;
      f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
      f.width = 960;
      f.height = 544;
      g_present_rc = sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
      g_presents++;
      sceDisplayWaitVblankStart();
    } else {
      sceKernelDelayThread(100 * 1000);
    }
  }
  return 0;
}

static int audio_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  pin_self(AFFINITY_CPU2, "audio-io");
  log_line("audio-io null backend");
  while (!g_cpu_done)
    sceKernelDelayThreadCB(1000 * 1000);
  return 0;
}

static SceUID spawn_sized(const char *name, SceKernelThreadEntry entry,
                          int affinity, int stack) {
  SceUID tid = sceKernelCreateThread(name, entry, 0x40, stack, 0, affinity, NULL);
  if (tid < 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "create %s failed: 0x%08x", name, (unsigned)tid);
    log_line(buf);
    return tid;
  }
  sceKernelStartThread(tid, 0, NULL);
  return tid;
}

static SceUID spawn(const char *name, SceKernelThreadEntry entry, int affinity) {
  return spawn_sized(name, entry, affinity, 0x40000);
}

static void log_fifo(const Chassis *ch) {
  char buf[256];
  /* The per-opcode census the hand-written parser kept existed to find out
   * where that parser was wrong. Dolphin's decoder is the reference, so what
   * is worth watching now is throughput and whether anything failed to
   * decode at all. */
  snprintf(buf, sizeof(buf),
           "gx bytes=%u cmds=%u draws=%u vtx=%u unk=%u pending=%u retrace=%u",
           (u32)gx_cp_bytes_in(ch->gx), (u32)gx_cp_commands(ch->gx),
           (u32)gx_cp_draws(ch->gx), (u32)gx_cp_vertices(ch->gx),
           (u32)gx_cp_unknown(ch->gx), gx_cp_pending(ch->gx),
           ch->flipper.retrace);
  log_line(buf);
  snprintf(buf, sizeof(buf),
           "ios opens=%u ioctls=%u ioctlvs=%u replies=%u hangs=%u last='%s'",
           ch->ios.opens, ch->ios.ioctls, ch->ios.ioctlvs, ch->ios.replies,
           ch->ios.hangs, ch->ios.last_path);
  log_line(buf);
  /* The console had no way to say "a file I wanted is not on the card". It is
   * the one failure only the device can have -- the host runs against the whole
   * extraction and the console runs against a staged subset -- and without the
   * name it reads as an emulation bug. It is not: it is a gap in the census. */
  snprintf(buf, sizeof(buf), "disc reads=%u bytes=%llu misses=%u",
           ch->disc.reads, (unsigned long long)ch->disc.bytes, ch->disc.misses);
  log_line(buf);
  for (u32 i = 0; i < ch->disc.miss_n; i++) {
    snprintf(buf, sizeof(buf), "  disc missing %s",
             ch->disc.entries[ch->disc.miss_idx[i]].path);
    log_line(buf);
  }
  snprintf(buf, sizeof(buf), "mmio unique=%u", ch->flipper.unique_n);
  for (u32 i = 0; i < ch->flipper.unique_n && i < 16; i++) {
    snprintf(buf, sizeof(buf), "  mmio 0x%08x", ch->flipper.unique_ea[i]);
    log_line(buf);
  }
}

/* Runs on its own thread, and the stack size is the point.
 *
 * The AOT translation turns a guest call into a host call, capped at
 * DOLRECOMP_C_MAX_CALL_DEPTH (24) frames of generated chunk code -- and a chunk
 * frame is not small. On the host that lands in an 8 MB stack and is invisible.
 * The Vita's main thread has a fraction of that, and overflowing it took the
 * whole process down without a core dump: every thread stopped at once, about
 * fifteen seconds in, at the point the boot first nests deeply. Eight
 * megabytes here matches what the host has always had. */
#define AOT_STACK (8 * 1024 * 1024)

static int run_aot(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  pin_self(AFFINITY_CPU0, "cpu-aot");
  if (!g_mem1 || !g_mem2) {
    log_line("guest RAM alloc failed");
    return 0;
  }

  static Chassis ch;
  char buf[352];
  if (!chassis_init(&ch, g_mem1, GC_MAIN_RAM_SIZE, g_mem2, WII_MEM2_SIZE,
                    chassis_log, NULL)) {
    log_line("chassis_init failed");
    return 0;
  }
  /* Not fatal on its own: the log says which tree is missing, and the boot gets
   * as far as the first disc read before it stalls. */
  if (!chassis_attach_media(&ch, GAME_ROOT, NAND_ROOT))
    log_line("media incomplete: need " GAME_ROOT " and " NAND_ROOT);
  if (!chassis_load_dol(&ch, DOL_PATH)) {
    log_line("need ux0:data/galaxyvita/sys/main.dol (user DOL, not in VPK)");
    return 0;
  }

  /* Graphics come up after the DOL is loaded and before the first block runs,
   * because the sink reads the live register file and the register file
   * belongs to the chassis. A failure here is logged and stepped over: the
   * boot is still worth running without a picture, and the log says why there
   * is not one. */
  {
    SceIoStat st;
    memset(&st, 0, sizeof(st));
    if (sceIoGetstat(NOGFX_FLAG, &st) >= 0) {
      log_line("gfx disabled by " NOGFX_FLAG);
      goto no_gfx;
    }
  }
  if (!gxm_init()) {
    snprintf(buf, sizeof(buf), "gxm init failed: %s", gxm_error());
    log_line(buf);
  } else {
    if (!gvsc_init(GXP_DIR)) {
      snprintf(buf, sizeof(buf), "shader cache: %s", gvsc_last_error());
      log_line(buf);
    }
    gx_gxm_set_frag_mode(read_fragdbg());
    if (!gx_gxm_init(gx_cp_regs(ch.gx))) {
      snprintf(buf, sizeof(buf), "gx backend failed: %s", gx_gxm_error());
      log_line(buf);
    } else {
      GxSink sink;
      unsigned compiled = 0, cached = 0, bad = 0;
      /* Every 16th of the first frames, which is enough to see the boot
       * sequence change without filling ux0: -- each frame is 2 MB. */
      read_dump_flag();
      gx_gxm_dump_frames(FRAME_DIR, g_dump_count, g_dump_stride);
      gx_gxm_sink(&sink);
      gx_cp_set_sink(ch.gx, &sink);
      g_gfx_ready = 1;
      gvsc_stats(&compiled, &cached, &bad);
      snprintf(buf, sizeof(buf),
               "gxm up %ux%u shaders compiled=%u cached=%u failed=%u",
               gxm_width(), gxm_height(), compiled, cached, bad);
      log_line(buf);
    }
  }

no_gfx:
  g_watch = &ch;
  read_btn_flag();
  chassis_set_input_fn(&ch, vita_pad, NULL);
  /* idle.txt runs the whole setup and then emulates nothing. Everything the
   * boot dies alongside -- every thread going quiet at once, the process still
   * resident, memory flat, no core dump -- is also what an app being suspended
   * from outside looks like, and the two are indistinguishable from the host.
   * If the watchdog keeps ticking here for a minute then nothing about this
   * app or this device stops it, and the cause is in what the chassis does. */
  {
    SceIoStat ist;
    memset(&ist, 0, sizeof(ist));
    if (sceIoGetstat(IDLE_FLAG, &ist) >= 0) {
      log_line("idle: emulating nothing, holding for 60s");
      log_flush();
      for (int i = 0; i < 60; i++)
        sceKernelDelayThread(1000 * 1000);
      log_line("idle: held 60s without being stopped");
      log_flush();
      return 0;
    }
  }
  {
    u32 cap = read_block_cap();
    char line[64];
    snprintf(line, sizeof(line), "block cap = %u", (unsigned)cap);
    log_line(line);
    chassis_run(&ch, cap);
  }

  snprintf(buf, sizeof(buf),
           "stop=%s hits=%u misses=%u fifo=%u ea=0x%08x pc=0x%08x "
           "mmio_r=%u mmio_w=%u smc=%u last=0x%08x",
           chassis_stop_name(ch.stop_reason), ch.dispatch_hits, ch.dispatch_misses,
           ch.fifo_writes, ch.fifo_ea, ch.fifo_pc, ch.mmio_reads, ch.mmio_writes,
           ch.smc_hits, ch.last_pc);
  log_line(buf);
  log_fifo(&ch);
  if (g_gfx_ready) {
    gx_gxm_report(buf, sizeof(buf));
    log_line(buf);
    gx_gxm_shutdown();
  }
  gvsc_shutdown();
  gxm_shutdown();
  chassis_shutdown(&ch);
  return 0;
}

int main(void) {
  ensure_data_dir();
  g_log_mtx = sceKernelCreateMutex("gv-log", 0, 0, NULL);
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

  log_line("GalaxyVita phase4 boot");
  log_line("clock: PSVshellPlus user-controlled");

  g_mem1 = vita_rw("gv-mem1", GC_MAIN_RAM_SIZE, &g_mem1_uid);
  g_mem2 = vita_rw("gv-mem2", WII_MEM2_SIZE, &g_mem2_uid);

  spawn("display", display_worker, AFFINITY_CPU1);
  spawn("audio-io", audio_worker, AFFINITY_CPU2);
  spawn("watchdog", watchdog, AFFINITY_CPU2);

  {
    SceUID cpu = spawn_sized("cpu-aot", run_aot, AFFINITY_CPU0, AOT_STACK);
    if (cpu >= 0)
      sceKernelWaitThreadEndCB(cpu, NULL, NULL);
    else
      log_line("could not create the AOT thread");
  }
  g_cpu_done = 1;
  sceKernelDelayThread(1500 * 1000);

  log_line("GalaxyVita phase4 exit");
  log_flush();
  sceKernelExitProcess(0);
  return 0;
}

/* GalaxyVita CoreHost Phase 4: AOT + Flipper HLE + vitaGL present. */
#include "chassis.h"
#include "present.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#define AFFINITY_CPU0 0x10000
#define AFFINITY_CPU1 0x20000
#define AFFINITY_CPU2 0x40000

#define LOG_PATH "ux0:data/galaxyvita/boot.log"
#define DOL_PATH "ux0:data/galaxyvita/sys/main.dol"
/* The disc and NAND trees the user's own extraction produces. Neither ships in
 * the VPK; see scripts/extract-disc.sh. */
#define GAME_ROOT "ux0:data/galaxyvita/game"
#define NAND_ROOT "ux0:data/galaxyvita/nand"
/* Blocks, not frames. The host chassis reaches the first real geometry a little
 * past 10M and settles around 60M; anything smaller stops inside the boot. */
#define BLOCK_CAP 60000000u

unsigned int _newlib_heap_size_user = 8u * 1024u * 1024u;

static SceUID g_log_mtx = -1;
static SceUID g_mem1_uid = -1;
static SceUID g_mem2_uid = -1;
static void *g_mem1 = NULL;
static void *g_mem2 = NULL;
static volatile int g_gpu_ready = 0;
static volatile int g_cpu_done = 0;

static void ensure_data_dir(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/galaxyvita", 0777);
  sceIoMkdir("ux0:data/galaxyvita/sys", 0777);
  sceIoMkdir("ux0:data/galaxyvita/game", 0777);
  sceIoMkdir("ux0:data/galaxyvita/nand", 0777);
}

static void log_line(const char *msg) {
  sceClibPrintf("%s\n", msg);
  if (g_log_mtx >= 0)
    sceKernelLockMutex(g_log_mtx, 1, NULL);
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, msg, strlen(msg));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
  }
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

static void vita_pad(BtInput *in, u32 frame, void *user) {
  (void)frame;
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
  in->buttons = b;
  in->nunchuk = 0;
  if (pad.buttons & SCE_CTRL_LTRIGGER)
    in->nunchuk |= BT_NUN_Z;
  if (pad.buttons & SCE_CTRL_TRIANGLE)
    in->nunchuk |= BT_NUN_C;
  in->stick_x = stick_axis(pad.lx, 0);
  in->stick_y = stick_axis(pad.ly, 1);
  in->ir_x = pointer_axis(pad.rx, 512, 512);
  in->ir_y = pointer_axis(pad.ry, 384, 384);
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

static int gpu_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  pin_self(AFFINITY_CPU1, "gpu-fifo");
  if (!gfx_present_init()) {
    char buf[192];
    snprintf(buf, sizeof(buf), "gfx present init failed (%s)", gfx_present_detail());
    log_line(buf);
    g_gpu_ready = -1;
  } else {
    char buf[192];
    snprintf(buf, sizeof(buf), "gpu-fifo backend=%s 960x544 %s", gfx_present_backend(),
             gfx_present_detail());
    log_line(buf);
    g_gpu_ready = 1;
  }
  unsigned frames = 0;
  while (!g_cpu_done || frames < 60) {
    gfx_present_frame();
    frames++;
  }
  {
    char buf[64];
    snprintf(buf, sizeof(buf), "gpu-fifo frames=%u", frames);
    log_line(buf);
  }
  return 0;
}

static int audio_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  pin_self(AFFINITY_CPU2, "audio-io");
  log_line("audio-io null backend");
  while (!g_cpu_done)
    sceKernelDelayThread(1000 * 1000);
  return 0;
}

static SceUID spawn(const char *name, SceKernelThreadEntry entry, int affinity) {
  SceUID tid = sceKernelCreateThread(name, entry, 0x40, 0x40000, 0, affinity, NULL);
  if (tid < 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "create %s failed: 0x%08x", name, (unsigned)tid);
    log_line(buf);
    return tid;
  }
  sceKernelStartThread(tid, 0, NULL);
  return tid;
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
  snprintf(buf, sizeof(buf), "mmio unique=%u", ch->flipper.unique_n);
  for (u32 i = 0; i < ch->flipper.unique_n && i < 16; i++) {
    snprintf(buf, sizeof(buf), "  mmio 0x%08x", ch->flipper.unique_ea[i]);
    log_line(buf);
  }
}

static void run_aot(void) {
  pin_self(AFFINITY_CPU0, "cpu-aot");
  if (!g_mem1 || !g_mem2) {
    log_line("guest RAM alloc failed");
    return;
  }

  static Chassis ch;
  if (!chassis_init(&ch, g_mem1, GC_MAIN_RAM_SIZE, g_mem2, WII_MEM2_SIZE,
                    chassis_log, NULL)) {
    log_line("chassis_init failed");
    return;
  }
  /* Not fatal on its own: the log says which tree is missing, and the boot gets
   * as far as the first disc read before it stalls. */
  if (!chassis_attach_media(&ch, GAME_ROOT, NAND_ROOT))
    log_line("media incomplete: need " GAME_ROOT " and " NAND_ROOT);
  if (!chassis_load_dol(&ch, DOL_PATH)) {
    log_line("need ux0:data/galaxyvita/sys/main.dol (user DOL, not in VPK)");
    return;
  }

  chassis_set_input_fn(&ch, vita_pad, NULL);
  chassis_run(&ch, BLOCK_CAP);

  char buf[280];
  snprintf(buf, sizeof(buf),
           "stop=%s hits=%u misses=%u fifo=%u ea=0x%08x pc=0x%08x "
           "mmio_r=%u mmio_w=%u smc=%u last=0x%08x",
           chassis_stop_name(ch.stop_reason), ch.dispatch_hits, ch.dispatch_misses,
           ch.fifo_writes, ch.fifo_ea, ch.fifo_pc, ch.mmio_reads, ch.mmio_writes,
           ch.smc_hits, ch.last_pc);
  log_line(buf);
  log_fifo(&ch);
  chassis_shutdown(&ch);
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

  spawn("gpu-fifo", gpu_worker, AFFINITY_CPU1);
  spawn("audio-io", audio_worker, AFFINITY_CPU2);

  int waits = 0;
  while (g_gpu_ready == 0 && waits < 100) {
    sceKernelDelayThread(100 * 1000);
    waits++;
  }

  run_aot();
  g_cpu_done = 1;
  sceKernelDelayThread(1500 * 1000);

  log_line("GalaxyVita phase4 exit");
  sceKernelExitProcess(0);
  return 0;
}

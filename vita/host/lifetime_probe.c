/* How long does an application get to live on this console?
 *
 * Every GalaxyVita boot dies 10-12 seconds after launch with every thread
 * going quiet at once, the process still resident and no core dump -- and it
 * does exactly the same with the emulator doing nothing at all, so nothing the
 * chassis does is responsible. This is the control: the smallest app that can
 * still report, launched through the same path, in the same slot, as the same
 * unsafe fself. If it dies at ten seconds too then the cause is the device or
 * the launch, and GalaxyVita is not implicated at all.
 *
 * It also carries the bisect, so the same binary answers the next question
 * without a rebuild. Flag files, each adding one thing GalaxyVita does:
 *
 *   probe_mem.txt      24 MB + 64 MB of USER_RW, the guest's MEM1 and MEM2
 *   probe_stack.txt    tick from an 8 MB thread, like the AOT thread
 *   probe_threads.txt  three more pinned workers, like the real boot
 *   probe_display.txt  present a black frame every vblank
 *
 * The display flag is not one of GalaxyVita's traits: it is the common
 * explanation for a console stopping homebrew -- that an app which never puts
 * anything on screen is not allowed to keep running. A heartbeat in the real
 * app did not help, but its sceDisplaySetFrameBuf return was never checked, so
 * that test proves nothing. This one reports the return code.
 *
 * Nothing here is emulation. The point is to add one suspect at a time and
 * watch for the run that stops reaching sixty seconds.
 */
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <string.h>

#define LOG_PATH "ux0:data/galaxyvita/lifetime.log"
#define DATA_DIR "ux0:data/galaxyvita"
#define FLAG_MEM DATA_DIR "/probe_mem.txt"
#define FLAG_STACK DATA_DIR "/probe_stack.txt"
#define FLAG_THREADS DATA_DIR "/probe_threads.txt"
#define FLAG_DISPLAY DATA_DIR "/probe_display.txt"
/* Same shape as probe_stack.txt -- main blocks in sceKernelWaitThreadEnd while
 * a worker ticks -- but with an ordinary stack. It splits the two things that
 * flag changes at once: the 8 MB stack, and main no longer being the thread
 * that runs. Whichever of these two dies is the answer. */
#define FLAG_SMALLSTACK DATA_DIR "/probe_smallstack.txt"
/* The candidate fix: a worker still does the work, but main polls in a delay
 * loop instead of parking in sceKernelWaitThreadEnd. This is the shape
 * GalaxyVita would have to adopt, so it is worth proving here first. */
#define FLAG_POLL DATA_DIR "/probe_poll.txt"
/* The launch path. vitacompanion's `launch` is
 * sceAppMgrLaunchAppByUri(0x20000, "psgm:play?titleid=..."), and an app it
 * starts is killed after ~14 s while the same binary started from its LiveArea
 * bubble runs as long as it likes. Homebrew launchers normally pass 0xFFFFF
 * for that flags word. If the kill is a property of how the process was
 * started rather than of the process, then starting a second time through a
 * clean path should shake it off -- so the first instance immediately
 * re-launches and exits, and the second one does the ticking.
 *
 * probe_relaunch.txt   re-launch once via sceAppMgrLaunchAppByUri(0xFFFFF)
 * probe_loadexec.txt   ... via sceAppMgrLoadExec("app0:eboot.bin") instead
 *
 * probe_reentry.txt is the guard, written by the first instance and removed by
 * the second. Without it a failed relaunch that still returns is an infinite
 * launch loop, which on this console means a power cycle and somebody at the
 * device. */
#define FLAG_RELAUNCH DATA_DIR "/probe_relaunch.txt"
#define FLAG_LOADEXEC DATA_DIR "/probe_loadexec.txt"
#define REENTRY_PATH DATA_DIR "/probe_reentry.txt"

#define AFFINITY_CPU0 0x10000
#define AFFINITY_CPU1 0x20000
#define AFFINITY_CPU2 0x40000

#define SECONDS 60
#define TITLEID "GVITA0001"

static SceUID g_fd = -1;
static volatile int g_done;
static volatile int g_ticks_done;

/* Opened once and held, and synced per line. A line that is still in the cache
 * when the app is stopped reads from the host exactly like a line that was
 * never written, which is the distinction this whole program exists to make. */
static void say(const char *msg) {
  if (g_fd < 0)
    return;
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "%s\n", msg);
  if (n > 0)
    sceIoWrite(g_fd, buf, (SceSize)n);
  sceIoSyncByFd(g_fd, 0);
}

static int flag(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  return sceIoGetstat(path, &st) >= 0;
}

/* Presents a black frame every vblank, and says whether the console accepted
 * it. The return code is the point: a heartbeat that is silently being refused
 * looks exactly like a heartbeat that did not help. */
static int display_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  SceUID uid = sceKernelAllocMemBlock("probe-fb",
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                      2 * 1024 * 1024, NULL);
  void *fb = NULL;
  if (uid >= 0 && sceKernelGetMemBlockBase(uid, &fb) < 0)
    fb = NULL;
  if (!fb) {
    say("display: no framebuffer");
    return 0;
  }
  memset(fb, 0, 2 * 1024 * 1024);
  int reported = 0;
  while (!g_done) {
    SceDisplayFrameBuf f;
    memset(&f, 0, sizeof(f));
    f.size = sizeof(f);
    f.base = fb;
    f.pitch = 960;
    f.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    f.width = 960;
    f.height = 544;
    int rc = sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
    if (reported < 3) {
      char buf[128];
      snprintf(buf, sizeof(buf), "display: setframebuf rc=0x%08x", (unsigned)rc);
      say(buf);
      reported++;
    }
    sceDisplayWaitVblankStart();
  }
  return 0;
}

static int idle_worker(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  while (!g_done)
    sceKernelDelayThread(1000 * 1000);
  return 0;
}

/* The ticking itself, so it can run either on the main thread or on a big one
 * without the loop differing between the two. */
static int tick_loop(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  char buf[256];
  for (int i = 1; i <= SECONDS; i++) {
    sceKernelDelayThread(1000 * 1000);
    SceUInt64 us = sceKernelGetProcessTimeWide();
    /* Whether vitacompanion's synthetic input reaches an application at all.
     * If it does here but cannot drive the LiveArea, then `press` is
     * app-only and the bubble is not reachable from the host. */
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    snprintf(buf, sizeof(buf), "tick %d proc=%llums buttons=0x%08x", i,
             (unsigned long long)(us / 1000ull), (unsigned)pad.buttons);
    say(buf);
  }
  say("survived 60s");
  g_ticks_done = 1;
  return 0;
}

int main(void) {
  sceIoMkdir(DATA_DIR, 0777);

  /* Second instance appends: the first instance's lines say which path it took
   * and are the only record of it, so truncating here would throw away half
   * the result. */
  int reentered = flag(REENTRY_PATH);
  g_fd = sceIoOpen(LOG_PATH,
                   SCE_O_WRONLY | SCE_O_CREAT |
                       (reentered ? SCE_O_APPEND : SCE_O_TRUNC),
                   0777);
  if (g_fd < 0)
    return 0;

  if (reentered) {
    sceIoRemove(REENTRY_PATH);
    say("re-entered: this instance was launched by the previous one");
  } else if (flag(FLAG_RELAUNCH) || flag(FLAG_LOADEXEC)) {
    int use_loadexec = flag(FLAG_LOADEXEC);
    SceUID g = sceIoOpen(REENTRY_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                         0777);
    if (g >= 0) {
      sceIoWrite(g, "1", 1);
      sceIoSyncByFd(g, 0);
      sceIoClose(g);
    }
    char buf[192];
    int rc;
    if (use_loadexec) {
      say("relaunching via sceAppMgrLoadExec(app0:eboot.bin)");
      rc = sceAppMgrLoadExec("app0:eboot.bin", NULL, NULL);
    } else {
      say("relaunching via sceAppMgrLaunchAppByUri(0xFFFFF)");
      rc = sceAppMgrLaunchAppByUri(0xFFFFF, "psgm:play?titleid=" TITLEID);
    }
    /* Reaching here means the process was not replaced. That is not the same
     * as the call failing: sceAppMgrLaunchAppByUri on the title that is
     * already running returns 0 and does nothing, which reads as success and
     * leaves the first instance ticking. Report the code and let the reader
     * decide; do not label rc=0 a failure. Either way, clear the guard so the
     * next run is not fooled into thinking it is the second instance. */
    snprintf(buf, sizeof(buf),
             "relaunch returned rc=0x%08x and did not replace this process",
             (unsigned)rc);
    say(buf);
    sceIoRemove(REENTRY_PATH);
  }

  int want_mem = flag(FLAG_MEM);
  int want_stack = flag(FLAG_STACK);
  int want_threads = flag(FLAG_THREADS);
  int want_display = flag(FLAG_DISPLAY);
  int want_small = flag(FLAG_SMALLSTACK);
  int want_poll = flag(FLAG_POLL);
  {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "lifetime probe up: mem=%d stack=%d threads=%d display=%d "
             "smallstack=%d poll=%d",
             want_mem, want_stack, want_threads, want_display, want_small,
             want_poll);
    say(buf);
  }

  if (want_display) {
    SceUID t = sceKernelCreateThread("probe-display", display_worker, 0x40,
                                     0x40000, 0, AFFINITY_CPU1, NULL);
    if (t >= 0)
      sceKernelStartThread(t, 0, NULL);
  }

  SceUID m1 = -1, m2 = -1;
  if (want_mem) {
    char buf[256];
    m1 = sceKernelAllocMemBlock("probe-mem1", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                24 * 1024 * 1024, NULL);
    m2 = sceKernelAllocMemBlock("probe-mem2", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                64 * 1024 * 1024, NULL);
    /* Touched, not just reserved: a budget is charged for pages that exist,
     * and a block nobody writes to may never cost anything. */
    void *b1 = NULL, *b2 = NULL;
    if (m1 >= 0)
      sceKernelGetMemBlockBase(m1, &b1);
    if (m2 >= 0)
      sceKernelGetMemBlockBase(m2, &b2);
    if (b1)
      memset(b1, 0, 24 * 1024 * 1024);
    if (b2)
      memset(b2, 0, 64 * 1024 * 1024);
    snprintf(buf, sizeof(buf), "mem: mem1=0x%08x mem2=0x%08x touched=%d",
             (unsigned)m1, (unsigned)m2, (b1 && b2) ? 1 : 0);
    say(buf);
  }

  if (want_threads) {
    for (int i = 0; i < 3; i++) {
      int aff = i == 0 ? AFFINITY_CPU0 : (i == 1 ? AFFINITY_CPU1 : AFFINITY_CPU2);
      SceUID t = sceKernelCreateThread("probe-idle", idle_worker, 0x40, 0x40000,
                                       0, aff, NULL);
      if (t >= 0)
        sceKernelStartThread(t, 0, NULL);
    }
    say("threads: three idle workers pinned");
  }

  if (want_stack || want_small || want_poll) {
    int stack_size = want_stack ? 8 * 1024 * 1024 : 0x40000;
    SceUID t = sceKernelCreateThread("probe-big", tick_loop, 0x40,
                                     stack_size, 0, AFFINITY_CPU0, NULL);
    if (t < 0) {
      char buf[128];
      snprintf(buf, sizeof(buf), "big thread failed: 0x%08x", (unsigned)t);
      say(buf);
    } else {
      say(want_poll ? "worker ticking, main polling in a delay loop"
                    : (want_stack ? "ticking from an 8 MB thread"
                                  : "ticking from a 256 KB thread, main waiting"));
      sceKernelStartThread(t, 0, NULL);
      if (want_poll) {
        while (!g_ticks_done)
          sceKernelDelayThread(100 * 1000);
      } else {
        sceKernelWaitThreadEnd(t, NULL, NULL);
      }
    }
  } else {
    say("ticking from the main thread");
    tick_loop(0, NULL);
  }

  g_done = 1;
  say("lifetime probe done");
  sceIoClose(g_fd);
  sceKernelExitProcess(0);
  return 0;
}

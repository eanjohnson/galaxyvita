/* Hello-Vita host: pin three threads, prove affinity, write a boot log.
 * Not the game runtime. */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <stdio.h>
#include <string.h>

/* Vita user-core affinity masks (not 1 << core). */
#define AFFINITY_CPU0 0x10000
#define AFFINITY_CPU1 0x20000
#define AFFINITY_CPU2 0x40000

#define LOG_PATH "ux0:data/galaxyvita/boot.log"

static void ensure_data_dir(void) {
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir("ux0:data/galaxyvita", 0777);
}

static void log_line(const char *msg) {
  sceClibPrintf("%s\n", msg);
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
  if (fd >= 0) {
    sceIoWrite(fd, msg, strlen(msg));
    sceIoWrite(fd, "\n", 1);
    sceIoClose(fd);
  }
}

static int worker(SceSize args, void *argp) {
  (void)args;
  const char *name = argp ? (const char *)argp : "?";
  SceKernelThreadInfo info;
  memset(&info, 0, sizeof(info));
  info.size = sizeof(info);
  sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);

  char buf[192];
  snprintf(buf, sizeof(buf),
           "thread=%s tid=0x%08x affinity=0x%x currentCpu=%d lastCpu=%d",
           name, (unsigned)sceKernelGetThreadId(),
           (unsigned)info.currentCpuAffinityMask, info.currentCpuId,
           info.lastExecutedCpuId);
  log_line(buf);
  sceKernelDelayThread(3 * 1000 * 1000);
  return 0;
}

static SceUID spawn(const char *name, int affinity) {
  SceUID tid = sceKernelCreateThread(name, worker, 0x40, 0x4000, 0, affinity, NULL);
  if (tid < 0) {
    char buf[96];
    snprintf(buf, sizeof(buf), "create %s failed: 0x%08x", name, (unsigned)tid);
    log_line(buf);
    return tid;
  }
  sceKernelStartThread(tid, strlen(name) + 1, (void *)name);
  return tid;
}

int main(void) {
  ensure_data_dir();
  /* Truncate previous log. */
  SceUID fd = sceIoOpen(LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0)
    sceIoClose(fd);

  log_line("GalaxyVita hello host boot");

  spawn("cpu-aot", AFFINITY_CPU0);
  spawn("gpu-fifo", AFFINITY_CPU1);
  spawn("audio-io", AFFINITY_CPU2);

  SceCtrlData pad;
  memset(&pad, 0, sizeof(pad));
  int frames = 0;
  while (frames < 60 * 30) { /* ~30s at 60 Hz, or until START */
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START)
      break;
    sceDisplayWaitVblankStart();
    frames++;
  }

  log_line("GalaxyVita hello host exit");
  sceKernelExitProcess(0);
  return 0;
}

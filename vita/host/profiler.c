#include "profiler.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#define PROF_FLAG "ux0:data/galaxyvita/prof.txt"

/* Open addressing, power of two, no eviction. 4096 distinct guest PCs is far
 * more than a phase of this game touches at block granularity -- the whole
 * dispatch census uses the same size -- and if it ever does fill, overflow is
 * counted rather than silently folded into a neighbour. */
#define PROF_SLOTS 4096u

static const Chassis *volatile g_ch;
static volatile int g_run;
static volatile int g_on;
static SceUID g_thread = -1;

static u32 g_pc[PROF_SLOTS];
static u32 g_hits[PROF_SLOTS];
static u32 g_phase[CHASSIS_PH_COUNT];
static u32 g_samples;
static u32 g_overflow;
static SceUInt64 g_t0;
static SceUInt64 g_t1;

static void note_pc(u32 pc) {
  /* Knuth's multiplicative hash; the low bits of a guest PC are always zero
   * mod four, so hashing has to use the high ones. */
  u32 i = (pc * 2654435761u) & (PROF_SLOTS - 1u);
  for (u32 n = 0; n < 32u; n++) {
    u32 j = (i + n) & (PROF_SLOTS - 1u);
    if (g_hits[j] == 0u) {
      g_pc[j] = pc;
      g_hits[j] = 1u;
      return;
    }
    if (g_pc[j] == pc) {
      g_hits[j]++;
      return;
    }
  }
  g_overflow++;
}

static int sampler(SceSize args, void *argp) {
  (void)args;
  (void)argp;
  g_t0 = (SceUInt64)sceKernelGetProcessTimeWide();
  while (g_run) {
    const Chassis *ch = g_ch;
    if (ch) {
      u8 ph = ch->phase;
      /* Read the phase first and the PC second. They can disagree by one
       * sample at a boundary; over tens of thousands of samples that is
       * noise, and the alternative -- a lock -- would put the emulation
       * thread's cost up by more than the answer is worth. */
      if (ph < CHASSIS_PH_COUNT)
        g_phase[ph]++;
      note_pc(ch->last_pc);
      g_samples++;
    }
    /* Asking for 1 ms does not reliably get 1 ms. That is why nothing here
     * assumes a rate: the report divides by the samples actually taken and
     * states the wall time they were taken over, so a sampler that drifted is
     * visible as a rate that is not 1000/s rather than as a wrong answer. */
    sceKernelDelayThread(1000);
  }
  g_t1 = (SceUInt64)sceKernelGetProcessTimeWide();
  return 0;
}

void gvprof_start(const Chassis *ch, int affinity) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  if (!ch || sceIoGetstat(PROF_FLAG, &st) < 0)
    return;
  g_ch = ch;
  g_run = 1;
  g_on = 1;
  g_thread = sceKernelCreateThread("gv-prof", sampler, 0x40, 0x4000, 0,
                                   affinity, NULL);
  if (g_thread < 0) {
    g_run = 0;
    g_on = 0;
    return;
  }
  sceKernelStartThread(g_thread, 0, NULL);
}

void gvprof_stop(void) {
  if (!g_on)
    return;
  g_run = 0;
  if (g_thread >= 0)
    sceKernelWaitThreadEnd(g_thread, NULL, NULL);
  g_thread = -1;
}

int gvprof_on(void) { return g_on; }

void gvprof_report(char *out, unsigned n) {
  static const char *k_name[CHASSIS_PH_COUNT] = {"guest", "idle", "disp",
                                                 "gx",    "ios",  "mmio"};
  SceUInt64 now = g_run ? (SceUInt64)sceKernelGetProcessTimeWide() : g_t1;
  u32 s = g_samples;
  u32 ms = g_t0 ? (u32)((now - g_t0) / 1000u) : 0u;
  int k = 0;
  if (!g_on) {
    snprintf(out, n, "prof off");
    return;
  }
  k = snprintf(out, n, "prof samples=%u over=%ums rate=%u/s ovf=%u", s, ms,
               ms ? (s * 1000u) / ms : 0u, g_overflow);
  for (u32 i = 0; i < CHASSIS_PH_COUNT && k > 0 && (unsigned)k < n; i++) {
    /* Tenths of a percent of samples taken. Integer maths on purpose: this
     * runs on the watchdog's thread beside card I/O and there is no reason
     * for it to pull in a float formatter. */
    u32 t = s ? (g_phase[i] * 1000u) / s : 0u;
    k += snprintf(out + k, n - (unsigned)k, " %s=%u.%u%%", k_name[i], t / 10u,
                  t % 10u);
  }
}

void gvprof_dump_pcs(const char *path, unsigned top) {
  SceUID fd;
  char line[96];
  u32 done = 0;
  if (!g_on)
    return;
  fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return;
  {
    int n = snprintf(line, sizeof(line), "# samples=%u overflow=%u\n",
                     g_samples, g_overflow);
    sceIoWrite(fd, line, (SceSize)n);
  }
  /* Selection sort by repeated max, `top` passes over 4096 slots. It is
   * O(top * slots) and it runs once a tick on an idle core; a real sort would
   * need a scratch array this thread has no reason to own. */
  while (done < top) {
    u32 best = 0;
    u32 bi = PROF_SLOTS;
    for (u32 i = 0; i < PROF_SLOTS; i++) {
      if (g_hits[i] == 0u || (g_hits[i] & 0x80000000u) != 0u)
        continue; /* empty, or already emitted on an earlier pass */
      if (g_hits[i] > best) {
        best = g_hits[i];
        bi = i;
      }
    }
    if (bi == PROF_SLOTS)
      break;
    {
      u32 t = g_samples ? (best * 1000u) / g_samples : 0u;
      int n = snprintf(line, sizeof(line), "0x%08x %8u %u.%u%%\n", g_pc[bi],
                       best, t / 10u, t % 10u);
      sceIoWrite(fd, line, (SceSize)n);
    }
    g_hits[bi] |= 0x80000000u; /* mark taken, restored below */
    done++;
  }
  for (u32 i = 0; i < PROF_SLOTS; i++)
    g_hits[i] &= 0x7FFFFFFFu;
  sceIoClose(fd);
}

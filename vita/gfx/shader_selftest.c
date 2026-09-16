/* Compiles the shaders Dolphin generates, on the hardware that has to run them.
 *
 * Drives SceShaccCg directly rather than through vitaShARK. The wrapper reports
 * every failure as "fatal internal error" at line -1 with no detail, which is
 * useless for deciding whether the Vita's Cg profile can express TEV's integer
 * maths. The raw API hands back a diagnostic list with real messages, codes and
 * source locations.
 *
 * Earlier dead ends, each of which cost a hardware cycle to find:
 *   - Linking the eboot without -Wl,-q yields a velf that vita-elf-create
 *     accepts silently, then jumps into padding and dies before main.
 *   - vitaGL's runtime GLSL translator handles a trivial shader but faults on
 *     a real Dolphin one, so that route is not available.
 *
 * Reads:  ux0:data/galaxyvita/shaders/<name>.cg
 * Writes: ux0:data/galaxyvita/shader_selftest.txt
 *
 * Runs unattended: no input, exits when done. */

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/shacccg.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitaGL.h>

#define SHADER_DIR "ux0:data/galaxyvita/shaders"
#define REPORT "ux0:data/galaxyvita/shader_selftest.txt"
/* vitaGL's location is checked first because the project's app puts it there. */
static const char *const k_module_paths[] = {
    "ur0:data/external/libshacccg.suprx", "ur0:data/libshacccg.suprx"};

unsigned int _newlib_heap_size_user = 300 * 1024 * 1024;

static SceUID g_report;
static SceShaccCgSourceFile g_source;

static void out(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (n > 0)
    sceIoWrite(g_report, line, (SceSize)n);
}

/* The compiler asks for its source through this rather than touching the file
 * system, so the whole program is handed over from memory. */
static SceShaccCgSourceFile *open_file(const char *fileName,
                                       const SceShaccCgSourceLocation *from,
                                       const SceShaccCgCompileOptions *opts,
                                       const char **errorString) {
  (void)fileName;
  (void)from;
  (void)opts;
  (void)errorString;
  return &g_source;
}

/* ShaccCg stops returning after ~5 compiles in one process, regardless of what
 * is being compiled -- the identical shader compiles five times and hangs on
 * the sixth. Accounting for every byte the compiler asked for showed why:
 * live memory returns to ~0 after each compile and the per-compile high-water
 * mark is under 2 MB, but each compile *churns* ~28 MB across many small
 * allocations. That is newlib arena fragmentation, not a leak -- newlib does
 * not return freed blocks to the system, so after ~5 compiles the 128 MB heap
 * can no longer satisfy a request and the compiler spins instead of failing.
 *
 * A bump arena reset between compiles was tried first and broke the very next
 * compile: ShaccCg retains a little state across calls (the accounting showed
 * ~1 KB per compile never handed back), so reclaiming wholesale pulls memory
 * out from under it.
 *
 * What works is keeping real free() semantics while making fragmentation
 * impossible: a segregated free list. Sizes round up to a power of two and
 * freed blocks go back on their bucket's list, so a steady-state workload
 * reuses the same blocks forever and the arena only ever grows to the true
 * live peak -- a couple of MB, not a couple of GB.
 */
#define GV_ARENA_BYTES (16u * 1024u * 1024u)
#define GV_MIN_SHIFT   4                    /* 16 B smallest bucket */
#define GV_BUCKETS     28                   /* up to 2 GB, far past what is used */
#define GV_HDR         16                   /* keeps the payload 16-byte aligned */

static unsigned char *gv_arena;
static unsigned long gv_off, gv_peak, gv_total, gv_reused;
static unsigned int gv_fail;
static void *gv_bucket[GV_BUCKETS];

static void *cg_malloc(unsigned int size) {
  unsigned int idx = GV_MIN_SHIFT;
  unsigned long need;
  unsigned char *blk;

  if (!gv_arena) { if (!gv_fail) gv_fail = 1u; return 0; }
  while (idx < GV_BUCKETS - 1 && (1ul << idx) < (unsigned long)size) idx++;

  if (gv_bucket[idx]) {                     /* reuse: no arena growth at all */
    blk = (unsigned char *)gv_bucket[idx];
    gv_bucket[idx] = *(void **)blk;
    gv_reused++;
    gv_total += (1ul << idx);
    *(unsigned long *)(blk + sizeof(void *)) = idx;
    return blk + GV_HDR;
  }

  need = (1ul << idx) + GV_HDR;
  if (gv_off + need > GV_ARENA_BYTES) {
    if (!gv_fail) gv_fail = size ? size : 1u;
    return 0;
  }
  blk = gv_arena + gv_off;
  gv_off += need;
  gv_total += (1ul << idx);
  if (gv_off > gv_peak) gv_peak = gv_off;
  *(unsigned long *)(blk + sizeof(void *)) = idx;
  return blk + GV_HDR;
}

static void cg_free(void *p) {
  unsigned char *blk;
  unsigned long idx;
  if (!p) return;
  blk = (unsigned char *)p - GV_HDR;
  idx = *(unsigned long *)(blk + sizeof(void *));
  if (idx >= GV_BUCKETS) return;            /* not ours; refuse to corrupt */
  *(void **)blk = gv_bucket[idx];
  gv_bucket[idx] = blk;
}

static char *slurp(const char *path, unsigned int *len) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  char *buf;
  int size;
  if (fd < 0)
    return NULL;
  size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  buf = (char *)malloc((size_t)size + 1u);
  if (!buf) {
    sceIoClose(fd);
    return NULL;
  }
  sceIoRead(fd, buf, (SceSize)size);
  buf[size] = '\0';
  sceIoClose(fd);
  *len = (unsigned int)size;
  return buf;
}

static const char *level_name(SceShaccCgDiagnosticLevel l) {
  switch (l) {
  case SCE_SHACCCG_DIAGNOSTIC_LEVEL_INFO:
    return "info";
  case SCE_SHACCCG_DIAGNOSTIC_LEVEL_WARNING:
    return "warn";
  default:
    return "error";
  }
}

/* Returns GXP size on success, 0 on failure; prints every diagnostic either
 * way, since the warnings are as interesting as the errors here. */
static unsigned int compile_cg(const char *src, unsigned int len,
                               SceShaccCgTargetProfile profile) {
  SceShaccCgCompileOptions opts;
  SceShaccCgCallbackList cb;
  const SceShaccCgCompileOutput *res;
  unsigned int size = 0;
  int i;

  g_source.fileName = "main.cg";
  g_source.text = src;
  g_source.size = len;

  memset(&opts, 0, sizeof(opts));
  sceShaccCgInitializeCompileOptions(&opts);
  opts.mainSourceFile = "main.cg";
  opts.targetProfile = profile;
  opts.entryFunctionName = "main";
  opts.optimizationLevel = 2;
  opts.useFastmath = 1;
  opts.useFastprecision = 1;
  opts.useFastint = 1;
  opts.locale = SCE_SHACCCG_ENGLISH;

  sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
  cb.openFile = open_file;

  res = sceShaccCgCompileProgram(&opts, &cb, 0);
  if (!res) {
    out("      compiler returned nothing\n");
    return 0;
  }
  for (i = 0; i < res->diagnosticCount; i++) {
    const SceShaccCgDiagnosticMessage *d = &res->diagnostics[i];
    unsigned line = d->location ? d->location->lineNumber : 0u;
    out("      %s[%u] line %u: %s\n", level_name(d->level), (unsigned)d->code,
        line, d->message ? d->message : "(no message)");
  }
  if (res->programData && res->programSize)
    size = res->programSize;
  sceShaccCgDestroyCompileOutput(res);
  out("      arena: used %luK of %uM, churn %luK, reused %lu%s\n",
      gv_peak >> 10, GV_ARENA_BYTES >> 20, gv_total >> 10, gv_reused,
      gv_fail ? " EXHAUSTED" : "");
  return size;
}

/* ShaccCg cannot survive an unbounded number of compiles in one process. The
 * bucketed allocator and a 300 MB heap pushed it from 5 to 9, and the trend --
 * roughly proportional to heap size -- says the compiler leaks something we do
 * not own and cannot free. Rather than keep chasing it, compile a bounded
 * batch per launch and resume on the next one. A fresh process reclaims
 * everything unconditionally.
 *
 * This is also the shape the real cache warmer needs: warming SMG's whole UID
 * set on first run was never going to fit in one process either.
 */
#define GV_BATCH    6
#define GV_MAX_CG   256
#define GV_NAME_MAX 128
#define PROGRESS "ux0:data/galaxyvita/selftest_progress.txt"
/* Written last, every launch. The host deletes it before launching and waits
 * for it to reappear -- without it the host cannot tell a finished batch from
 * the previous batch's report still sitting on disk, and "relaunch until done"
 * silently spins without ever starting the app again. */
#define BATCH_MARK "ux0:data/galaxyvita/selftest_batch.txt"

static char gv_names[GV_MAX_CG][GV_NAME_MAX];
static int gv_count;

static void gv_collect(void) {
  SceIoDirent ent;
  SceUID dir = sceIoDopen(SHADER_DIR);
  if (dir < 0) return;
  memset(&ent, 0, sizeof(ent));
  while (sceIoDread(dir, &ent) > 0 && gv_count < GV_MAX_CG) {
    size_t n = strlen(ent.d_name);
    if (n >= 4 && n < GV_NAME_MAX && strcmp(ent.d_name + n - 3, ".cg") == 0)
      strcpy(gv_names[gv_count++], ent.d_name);
    memset(&ent, 0, sizeof(ent));
  }
  sceIoDclose(dir);
  /* sceIoDread order is not guaranteed stable, and resuming by index requires
   * that it is. Sort so every launch sees the same sequence. */
  {
    int a, b;
    for (a = 1; a < gv_count; a++) {
      char tmp[GV_NAME_MAX];
      strcpy(tmp, gv_names[a]);
      for (b = a - 1; b >= 0 && strcmp(gv_names[b], tmp) > 0; b--)
        strcpy(gv_names[b + 1], gv_names[b]);
      strcpy(gv_names[b + 1], tmp);
    }
  }
}

static void gv_progress_read(int *idx, int *ok, int *failed, int *skipped) {
  char buf[128];
  int n;
  SceUID f = sceIoOpen(PROGRESS, SCE_O_RDONLY, 0777);
  *idx = *ok = *failed = *skipped = 0;
  if (f < 0) return;
  n = sceIoRead(f, buf, sizeof(buf) - 1);
  sceIoClose(f);
  if (n <= 0) return;
  buf[n] = 0;
  /* Hand-rolled rather than sscanf: newlib's scanf family is easy to get
   * stubbed out by the link on this toolchain, and a silent parse failure here
   * looks exactly like "no progress file" -- which restarts the batch forever. */
  {
    char *q = buf;
    int *out_fields[4];
    int k;
    out_fields[0] = idx; out_fields[1] = ok;
    out_fields[2] = failed; out_fields[3] = skipped;
    for (k = 0; k < 4; k++) {
      while (*q == ' ' || *q == '\n' || *q == '\r' || *q == '\t') q++;
      *out_fields[k] = (int)strtol(q, &q, 10);
    }
  }
}

static void gv_progress_write(int idx, int ok, int failed, int skipped) {
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "%d %d %d %d\n", idx, ok, failed, skipped);
  SceUID f = sceIoOpen(PROGRESS, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (f < 0) return;
  sceIoWrite(f, buf, (SceSize)n);
  sceIoClose(f);
}

int main(void) {
  SceUID mod = -1;
  unsigned i;
  int idx = 0, ok = 0, failed = 0, skipped = 0, done_this_launch = 0;

  sceIoMkdir("ux0:data/galaxyvita", 0777);
  gv_progress_read(&idx, &ok, &failed, &skipped);

  /* First launch truncates the report; a resume appends to it. */
  g_report = sceIoOpen(REPORT,
                       idx == 0 ? (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC)
                                : (SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND),
                       0777);
  if (g_report < 0) {
    sceKernelExitProcess(1);
    return 1;
  }

  (void)vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024,
                        SCE_GXM_MULTISAMPLE_NONE);

  for (i = 0; i < sizeof(k_module_paths) / sizeof(k_module_paths[0]); i++) {
    mod = sceKernelLoadStartModule(k_module_paths[i], 0, NULL, 0, NULL, NULL);
    if (mod >= 0)
      break;
  }

  {
    const char *ver;
    gv_arena = (unsigned char *)malloc(GV_ARENA_BYTES);
    if (!gv_arena) {
      out("shader_selftest: could not reserve the %u MB compiler arena\n",
          GV_ARENA_BYTES >> 20);
      sceIoClose(g_report);
      return 1;
    }
    sceShaccCgSetDefaultAllocator(cg_malloc, cg_free);
    ver = sceShaccCgGetVersionString();
    if (idx == 0)
      out("shader_selftest: compiler %s\n\n", ver ? ver : "UNAVAILABLE");
    if (!ver) {
      out("No runtime compiler. Extract libshacccg.suprx from this console's\n"
          "firmware; it is not distributable and is not in the VPK.\n");
      sceIoClose(g_report);
      sceKernelExitProcess(0);
      return 0;
    }
  }

  gv_collect();
  if (idx)
    out("  [resuming at %d of %d]\n", idx, gv_count);
  if (gv_count == 0) {
    out("shader_selftest: no shaders at %s\n", SHADER_DIR);
    out("\nshader_selftest: %d ok, %d failed, %d skipped\n", ok, failed,
        skipped);
    sceIoClose(g_report);
    sceKernelExitProcess(0);
    return 0;
  }

  while (idx < gv_count && done_this_launch < GV_BATCH) {
    const char *name = gv_names[idx];
    char path[512];
    SceShaccCgTargetProfile profile;
    unsigned int len = 0, sz;
    char *src;

    if (strstr(name, ".frag."))
      profile = SCE_SHACCCG_PROFILE_FP;
    else if (strstr(name, ".vert."))
      profile = SCE_SHACCCG_PROFILE_VP;
    else {
      skipped++;
      idx++;
      continue;
    }

    snprintf(path, sizeof(path), "%s/%s", SHADER_DIR, name);
    src = slurp(path, &len);
    if (!src) {
      skipped++;
      idx++;
      continue;
    }

    out("  %s (%u B)\n", name, len);
    sz = compile_cg(src, len, profile);
    if (sz) {
      ok++;
      out("    ok -> %u B gxp\n", sz);
    } else {
      failed++;
      out("    FAIL\n");
    }
    free(src);
    idx++;
    done_this_launch++;
    /* Record after every shader: if the compiler wedges mid-batch, the next
     * launch resumes past the one that wedged rather than repeating it. */
    gv_progress_write(idx, ok, failed, skipped);
  }

  if (idx >= gv_count)
    out("\nshader_selftest: %d ok, %d failed, %d skipped\n", ok, failed,
        skipped);
  else
    out("  -- batch done, %d of %d compiled; relaunch to continue --\n", idx,
        gv_count);

  sceShaccCgReleaseCompiler();
  sceIoClose(g_report);
  {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%d %d\n", idx, gv_count);
    SceUID f =
        sceIoOpen(BATCH_MARK, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (f >= 0) {
      sceIoWrite(f, buf, (SceSize)n);
      sceIoClose(f);
    }
  }
  sceKernelExitProcess(0);
  return 0;
}

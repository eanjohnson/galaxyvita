#include "shader_cache.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/shacccg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* vitaGL's copy is checked first because the project's own installer puts one
 * there; the raw firmware location is the fallback. */
static const char *const k_module_paths[] = {"ur0:data/external/libshacccg.suprx",
                                             "ur0:data/libshacccg.suprx"};

enum { GVSC_MAX_PROGRAMS = 256 };
/* ShaccCg stops returning after 5-9 compiles in one process no matter what it
 * is compiling. Nine is the measured ceiling with the allocator below and a
 * large heap; six leaves headroom. */
enum { GVSC_COMPILE_BUDGET = 6 };

typedef struct {
  char name[96];
  SceGxmProgram *program;
} Entry;

static Entry g_entry[GVSC_MAX_PROGRAMS];
static int g_entries;
static char g_dir[128];
static int g_have_compiler;
static int g_budget = GVSC_COMPILE_BUDGET;
static char g_error[512];
static unsigned g_compiled, g_cached, g_failed;
static SceShaccCgSourceFile g_source;

/* ------------------------------------------------------------------ */
/* The compiler's allocator.
 *
 * ShaccCg churns ~28 MB across many small allocations per compile while its
 * live high-water mark stays under 2 MB. newlib never returns freed blocks to
 * the system, so that churn fragments the arena until an allocation cannot be
 * satisfied -- and the compiler spins rather than failing, which is how this
 * first showed up as a hang with no diagnostic.
 *
 * A segregated free list keeps real free() semantics while making
 * fragmentation impossible: sizes round up to a power of two and freed blocks
 * go back on their bucket. Measured flat at 5,562 KB -> 5,569 KB across six
 * compiles while absorbing 305 MB of churn, with byte-identical output each
 * time. (A bump arena reset between compiles was tried first and broke the
 * next compile: ShaccCg retains about a kilobyte across calls.) */
#define GV_ARENA_BYTES (16u * 1024u * 1024u)
#define GV_MIN_SHIFT 4  /* 16 B smallest bucket */
#define GV_BUCKETS 28   /* up to 2 GB, far past what is used */
#define GV_HDR 16       /* keeps the payload 16-byte aligned */

static unsigned char *gv_arena;
static unsigned long gv_off, gv_peak, gv_total, gv_reused;
static unsigned int gv_fail;
static void *gv_bucket[GV_BUCKETS];

static void *cg_malloc(unsigned int size) {
  unsigned int idx = GV_MIN_SHIFT;
  unsigned long need;
  unsigned char *blk;

  if (!gv_arena) {
    if (!gv_fail)
      gv_fail = 1u;
    return 0;
  }
  while (idx < GV_BUCKETS - 1 && (1ul << idx) < (unsigned long)size)
    idx++;

  if (gv_bucket[idx]) { /* reuse: no arena growth at all */
    blk = (unsigned char *)gv_bucket[idx];
    gv_bucket[idx] = *(void **)blk;
    gv_reused++;
    gv_total += (1ul << idx);
    *(unsigned long *)(blk + sizeof(void *)) = idx;
    return blk + GV_HDR;
  }

  need = (1ul << idx) + GV_HDR;
  if (gv_off + need > GV_ARENA_BYTES) {
    if (!gv_fail)
      gv_fail = size ? size : 1u;
    return 0;
  }
  blk = gv_arena + gv_off;
  gv_off += need;
  gv_total += (1ul << idx);
  if (gv_off > gv_peak)
    gv_peak = gv_off;
  *(unsigned long *)(blk + sizeof(void *)) = idx;
  return blk + GV_HDR;
}

static void cg_free(void *p) {
  unsigned char *blk;
  unsigned long idx;
  if (!p)
    return;
  blk = (unsigned char *)p - GV_HDR;
  idx = *(unsigned long *)(blk + sizeof(void *));
  if (idx >= GV_BUCKETS)
    return; /* not ours; refuse to corrupt */
  *(void **)blk = gv_bucket[idx];
  gv_bucket[idx] = blk;
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

/* ------------------------------------------------------------------ */

static void cache_path(char *out, unsigned n, const char *name) {
  snprintf(out, n, "%s/%s.gxp", g_dir, name);
}

static SceGxmProgram *load_blob(const char *path) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  int size;
  void *buf;
  if (fd < 0)
    return NULL;
  size = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (size <= 0) {
    sceIoClose(fd);
    return NULL;
  }
  buf = malloc((size_t)size);
  if (!buf) {
    sceIoClose(fd);
    return NULL;
  }
  if (sceIoRead(fd, buf, (SceSize)size) != size) {
    sceIoClose(fd);
    free(buf);
    return NULL;
  }
  sceIoClose(fd);
  /* A truncated or half-written blob would otherwise be registered and taken
   * as valid; the patcher's failure mode for that is a GPU fault, not an
   * error return. */
  if (sceGxmProgramCheck((const SceGxmProgram *)buf) < 0) {
    free(buf);
    return NULL;
  }
  return (SceGxmProgram *)buf;
}

static void store_blob(const char *path, const void *data, unsigned size) {
  /* Written to a temporary and renamed, so a launch interrupted mid-write
   * leaves no blob that would fail sceGxmProgramCheck on the next run. */
  char tmp[208];
  SceUID fd;
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return;
  if (sceIoWrite(fd, data, (SceSize)size) != (int)size) {
    sceIoClose(fd);
    sceIoRemove(tmp);
    return;
  }
  sceIoClose(fd);
  sceIoRemove(path);
  sceIoRename(tmp, path);
}

static SceGxmProgram *compile(const char *src, int fragment, unsigned *out_size) {
  SceShaccCgCompileOptions opts;
  SceShaccCgCallbackList cb;
  const SceShaccCgCompileOutput *res;
  SceGxmProgram *program = NULL;
  int i;
  unsigned used = 0;

  g_source.fileName = "main.cg";
  g_source.text = src;
  g_source.size = (unsigned int)strlen(src);

  memset(&opts, 0, sizeof(opts));
  sceShaccCgInitializeCompileOptions(&opts);
  opts.mainSourceFile = "main.cg";
  opts.targetProfile = fragment ? SCE_SHACCCG_PROFILE_FP : SCE_SHACCCG_PROFILE_VP;
  opts.entryFunctionName = "main";
  opts.optimizationLevel = 2;
  opts.useFastmath = 1;
  opts.useFastprecision = 1;
  /* The TEV code clamps values whose signed wrap-around is the hardware
   * behaviour, so the integer path has to stay integer. */
  opts.useFastint = 1;
  opts.locale = SCE_SHACCCG_ENGLISH;

  sceShaccCgInitializeCallbackList(&cb, SCE_SHACCCG_TRIVIAL);
  cb.openFile = open_file;

  res = sceShaccCgCompileProgram(&opts, &cb, 0);
  if (!res) {
    snprintf(g_error, sizeof(g_error), "compiler returned nothing");
    return NULL;
  }
  g_error[0] = '\0';
  for (i = 0; i < res->diagnosticCount; i++) {
    const SceShaccCgDiagnosticMessage *d = &res->diagnostics[i];
    unsigned line = d->location ? d->location->lineNumber : 0u;
    if (d->level == SCE_SHACCCG_DIAGNOSTIC_LEVEL_INFO)
      continue;
    used += (unsigned)snprintf(g_error + used, sizeof(g_error) - used,
                               "%s%u:%u: %s", used ? " | " : "",
                               (unsigned)d->code, line,
                               d->message ? d->message : "(no message)");
    if (used >= sizeof(g_error) - 1u)
      break;
  }
  if (res->programData && res->programSize) {
    program = (SceGxmProgram *)malloc(res->programSize);
    if (program) {
      memcpy(program, res->programData, res->programSize);
      *out_size = res->programSize;
    }
  } else if (!g_error[0]) {
    snprintf(g_error, sizeof(g_error), "no program and no diagnostic");
  }
  sceShaccCgDestroyCompileOutput(res);
  return program;
}

/* ------------------------------------------------------------------ */

int gvsc_init(const char *dir) {
  const char *version;
  unsigned i;

  snprintf(g_dir, sizeof(g_dir), "%s", dir);
  sceIoMkdir(g_dir, 0777);

  for (i = 0; i < sizeof(k_module_paths) / sizeof(k_module_paths[0]); i++) {
    if (sceKernelLoadStartModule(k_module_paths[i], 0, NULL, 0, NULL, NULL) >= 0)
      break;
  }

  gv_arena = (unsigned char *)malloc(GV_ARENA_BYTES);
  if (!gv_arena) {
    snprintf(g_error, sizeof(g_error), "no room for the %u MB compiler arena",
             GV_ARENA_BYTES >> 20);
    return 0;
  }
  sceShaccCgSetDefaultAllocator(cg_malloc, cg_free);
  version = sceShaccCgGetVersionString();
  if (!version) {
    snprintf(g_error, sizeof(g_error),
             "no runtime compiler; extract libshacccg.suprx from this "
             "console's firmware (it is not distributable, so it is not in "
             "the VPK)");
    return 0;
  }
  g_have_compiler = 1;
  return 1;
}

const SceGxmProgram *gvsc_program(const char *name, const char *src, int fragment) {
  char path[192];
  SceGxmProgram *program;
  unsigned size = 0;
  int i;

  for (i = 0; i < g_entries; i++)
    if (strcmp(g_entry[i].name, name) == 0)
      return g_entry[i].program;
  if (g_entries >= GVSC_MAX_PROGRAMS) {
    snprintf(g_error, sizeof(g_error), "program table full");
    return NULL;
  }

  cache_path(path, sizeof(path), name);
  program = load_blob(path);
  if (program) {
    g_cached++;
  } else {
    if (!src || !g_have_compiler) {
      if (!g_error[0])
        snprintf(g_error, sizeof(g_error), "%s not cached and no compiler", name);
      g_failed++;
      return NULL;
    }
    if (g_budget <= 0) {
      snprintf(g_error, sizeof(g_error),
               "compile budget spent this launch; %s waits for the next", name);
      return NULL;
    }
    g_budget--;
    program = compile(src, fragment, &size);
    if (!program) {
      g_failed++;
      return NULL;
    }
    store_blob(path, program, size);
    g_compiled++;
  }

  snprintf(g_entry[g_entries].name, sizeof(g_entry[g_entries].name), "%s", name);
  g_entry[g_entries].program = program;
  g_entries++;
  return program;
}

int gvsc_budget_left(void) { return g_have_compiler ? g_budget : 0; }

const char *gvsc_last_error(void) { return g_error; }

void gvsc_stats(unsigned *compiled, unsigned *cached, unsigned *failed) {
  if (compiled)
    *compiled = g_compiled;
  if (cached)
    *cached = g_cached;
  if (failed)
    *failed = g_failed;
}

void gvsc_shutdown(void) {
  if (g_have_compiler)
    sceShaccCgReleaseCompiler();
  g_have_compiler = 0;
  /* Compiled programs are copied out into their own allocations, so the
   * compiler's arena owns nothing the caller still holds and can go. The
   * programs themselves stay: the patcher has them registered. */
  free(gv_arena);
  gv_arena = NULL;
  memset(gv_bucket, 0, sizeof(gv_bucket));
  gv_off = 0;
}

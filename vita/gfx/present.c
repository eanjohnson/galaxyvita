#include "present.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>

static int g_vgl = 0;
static const char *g_backend = "none";
static char g_detail[160] = "";

static const char *k_shacc_vgl = "ur0:data/external/libshacccg.suprx";
static const char *k_shacc_src = "ur0:data/libshacccg.suprx";

static int file_ok(const char *path) {
  SceIoStat st;
  memset(&st, 0, sizeof(st));
  return sceIoGetstat(path, &st) >= 0 && st.st_size > 0;
}

static int copy_file(const char *src, const char *dst) {
  SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
  if (in < 0)
    return 0;
  SceUID out = sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (out < 0) {
    sceIoClose(in);
    return 0;
  }
  char buf[16 * 1024];
  int n;
  int ok = 1;
  while ((n = sceIoRead(in, buf, sizeof(buf))) > 0) {
    if (sceIoWrite(out, buf, n) != n) {
      ok = 0;
      break;
    }
  }
  if (n < 0)
    ok = 0;
  sceIoClose(in);
  sceIoClose(out);
  return ok;
}

static int ensure_libshacc(void) {
  if (file_ok(k_shacc_vgl))
    return 1;
  sceIoMkdir("ur0:data", 0777);
  sceIoMkdir("ur0:data/external", 0777);
  if (!file_ok(k_shacc_src)) {
    snprintf(g_detail, sizeof(g_detail), "no libshacccg at %s or %s", k_shacc_src,
             k_shacc_vgl);
    return 0;
  }
  if (!copy_file(k_shacc_src, k_shacc_vgl)) {
    snprintf(g_detail, sizeof(g_detail), "copy libshacccg to external failed");
    return 0;
  }
  return file_ok(k_shacc_vgl);
}

int gfx_present_init(void) {
  int have_shacc = ensure_libshacc();
  /*
   * vitaGL's GLboolean is not success/fail. vglInitWithCustomSizes returns
   * GL_TRUE only if it had to shrink the requested resolution; a normal
   * 960×544 init returns GL_FALSE. Samples ignore the return.
   */
  (void)vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024,
                        SCE_GXM_MULTISAMPLE_NONE);
  g_vgl = 1;
  g_backend = "vitaGL";
  snprintf(g_detail, sizeof(g_detail), "shacc=%d", have_shacc);
  glViewport(0, 0, 960, 544);
  glClearColor(0.05f, 0.10f, 0.16f, 1.0f);
  return 1;
}

void gfx_present_frame(void) {
  if (!g_vgl)
    return;
  glClear(GL_COLOR_BUFFER_BIT);
  vglSwapBuffers(GL_FALSE);
}

const char *gfx_present_backend(void) { return g_backend; }

const char *gfx_present_detail(void) { return g_detail; }

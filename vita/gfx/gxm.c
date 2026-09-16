#include "gxm.h"

#include <psp2/display.h>
#include <psp2/kernel/sysmem.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DISPLAY_WIDTH 960u
#define DISPLAY_HEIGHT 544u
#define DISPLAY_STRIDE 1024u /* the display controller wants a 64-pixel multiple */
#define DISPLAY_BUFFERS 2u

#define ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

typedef struct {
  void *address;
} DisplayData;

static int g_up;
static char g_error[192];

static SceGxmContext *g_context;
static SceGxmRenderTarget *g_target;
static SceGxmShaderPatcher *g_patcher;

static void *g_host_mem;
static SceUID g_vdm_uid, g_vertex_uid, g_fragment_uid, g_frag_usse_uid;
static SceUID g_depth_uid, g_patch_buf_uid, g_patch_vert_uid, g_patch_frag_uid;

static void *g_display_buffer[DISPLAY_BUFFERS];
static SceUID g_display_uid[DISPLAY_BUFFERS];
static SceGxmColorSurface g_display_surface[DISPLAY_BUFFERS];
static SceGxmSyncObject *g_display_sync[DISPLAY_BUFFERS];
static SceGxmDepthStencilSurface g_depth_surface;

static unsigned g_back;   /* the buffer being rendered into */
static unsigned g_front;  /* the buffer on screen */
static unsigned g_frames; /* total scenes ended */

static void fail(const char *what, int rc) {
  if (!g_error[0])
    snprintf(g_error, sizeof(g_error), "%s: 0x%08x", what, (unsigned)rc);
}

void *gxm_alloc(SceKernelMemBlockType type, unsigned int size,
                unsigned int alignment, unsigned int attribs, SceUID *uid) {
  void *mem = NULL;
  SceUID block;

  /* The two block types round differently, and getting this wrong is an
   * allocation failure rather than a warning. */
  if (type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW)
    size = ALIGN_UP(size, 256u * 1024u);
  else
    size = ALIGN_UP(size, 4u * 1024u);

  block = sceKernelAllocMemBlock("gv-gxm", type, size, NULL);
  if (block < 0) {
    fail("AllocMemBlock", block);
    *uid = -1;
    return NULL;
  }
  if (sceKernelGetMemBlockBase(block, &mem) < 0) {
    sceKernelFreeMemBlock(block);
    fail("GetMemBlockBase", -1);
    *uid = -1;
    return NULL;
  }
  if (alignment && ((uintptr_t)mem & (alignment - 1u)) != 0u) {
    sceKernelFreeMemBlock(block);
    fail("alignment", (int)alignment);
    *uid = -1;
    return NULL;
  }
  if (sceGxmMapMemory(mem, size, (SceGxmMemoryAttribFlags)attribs) < 0) {
    sceKernelFreeMemBlock(block);
    fail("MapMemory", -1);
    *uid = -1;
    return NULL;
  }
  *uid = block;
  return mem;
}

void gxm_free(SceUID uid) {
  void *mem = NULL;
  if (uid < 0)
    return;
  if (sceKernelGetMemBlockBase(uid, &mem) >= 0)
    sceGxmUnmapMemory(mem);
  sceKernelFreeMemBlock(uid);
}

/* USSE memory is mapped through its own calls and must be unmapped through
 * them; sceGxmUnmapMemory does not know about it. */
static void usse_free(SceUID uid, int fragment) {
  void *mem = NULL;
  if (uid < 0)
    return;
  if (sceKernelGetMemBlockBase(uid, &mem) >= 0) {
    if (fragment)
      sceGxmUnmapFragmentUsseMemory(mem);
    else
      sceGxmUnmapVertexUsseMemory(mem);
  }
  sceKernelFreeMemBlock(uid);
}

static void *usse_alloc(unsigned int size, SceUID *uid, unsigned int *usse_offset,
                        int fragment) {
  void *mem = NULL;
  SceUID block;
  size = ALIGN_UP(size, 4u * 1024u);
  block = sceKernelAllocMemBlock("gv-usse", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                 size, NULL);
  if (block < 0) {
    fail("usse AllocMemBlock", block);
    *uid = -1;
    return NULL;
  }
  sceKernelGetMemBlockBase(block, &mem);
  if (fragment)
    sceGxmMapFragmentUsseMemory(mem, size, usse_offset);
  else
    sceGxmMapVertexUsseMemory(mem, size, usse_offset);
  *uid = block;
  return mem;
}

static void display_callback(const void *data) {
  const DisplayData *d = (const DisplayData *)data;
  SceDisplayFrameBuf fb;
  memset(&fb, 0, sizeof(fb));
  fb.size = sizeof(fb);
  fb.base = d->address;
  fb.pitch = DISPLAY_STRIDE;
  fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  fb.width = DISPLAY_WIDTH;
  fb.height = DISPLAY_HEIGHT;
  sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
  sceDisplayWaitVblankStart();
}

/* The patcher's buffer and USSE heaps are handed over as fixed budgets rather
 * than through alloc callbacks: the shader set is bounded by the game's UID
 * census, so a growable heap would only turn "ran out" into a harder-to-read
 * failure further from the cause. Only the small host-side allocations go
 * through callbacks, because the patcher insists on them. */
static void *patcher_host_alloc(void *user, unsigned int size) {
  (void)user;
  return malloc(size);
}

static void patcher_host_free(void *user, void *mem) {
  (void)user;
  free(mem);
}

int gxm_init(void) {
  SceGxmInitializeParams init;
  SceGxmContextParams ctx;
  SceGxmRenderTargetParams rt;
  SceGxmShaderPatcherParams patch;
  void *vdm_ring, *vertex_ring, *fragment_ring, *frag_usse_ring;
  void *patch_buf, *patch_vert, *patch_frag;
  unsigned int frag_usse_offset = 0, patch_vert_offset = 0, patch_frag_offset = 0;
  unsigned int aligned_w, aligned_h, sample_count, i;
  void *depth_mem;
  int rc;

  if (g_up)
    return 1;
  g_error[0] = '\0';

  memset(&init, 0, sizeof(init));
  init.flags = 0;
  init.displayQueueMaxPendingCount = DISPLAY_BUFFERS - 1u;
  init.displayQueueCallback = display_callback;
  init.displayQueueCallbackDataSize = sizeof(DisplayData);
  init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
  rc = sceGxmInitialize(&init);
  if (rc < 0) {
    fail("sceGxmInitialize", rc);
    return 0;
  }

  vdm_ring = gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                       SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, 4,
                       SCE_GXM_MEMORY_ATTRIB_READ, &g_vdm_uid);
  vertex_ring = gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                          SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, 4,
                          SCE_GXM_MEMORY_ATTRIB_READ, &g_vertex_uid);
  fragment_ring = gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                            SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, 4,
                            SCE_GXM_MEMORY_ATTRIB_READ, &g_fragment_uid);
  frag_usse_ring = usse_alloc(SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
                              &g_frag_usse_uid, &frag_usse_offset, 1);
  g_host_mem = malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
  if (!vdm_ring || !vertex_ring || !fragment_ring || !frag_usse_ring ||
      !g_host_mem) {
    fail("context memory", 0);
    return 0;
  }

  memset(&ctx, 0, sizeof(ctx));
  ctx.hostMem = g_host_mem;
  ctx.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
  ctx.vdmRingBufferMem = vdm_ring;
  ctx.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
  ctx.vertexRingBufferMem = vertex_ring;
  ctx.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
  ctx.fragmentRingBufferMem = fragment_ring;
  ctx.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
  ctx.fragmentUsseRingBufferMem = frag_usse_ring;
  ctx.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
  ctx.fragmentUsseRingBufferOffset = frag_usse_offset;
  rc = sceGxmCreateContext(&ctx, &g_context);
  if (rc < 0) {
    fail("sceGxmCreateContext", rc);
    return 0;
  }

  memset(&rt, 0, sizeof(rt));
  rt.width = DISPLAY_WIDTH;
  rt.height = DISPLAY_HEIGHT;
  rt.scenesPerFrame = 1;
  rt.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
  rt.driverMemBlock = -1;
  rc = sceGxmCreateRenderTarget(&rt, &g_target);
  if (rc < 0) {
    fail("sceGxmCreateRenderTarget", rc);
    return 0;
  }

  for (i = 0; i < DISPLAY_BUFFERS; i++) {
    g_display_buffer[i] =
        gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                  4u * DISPLAY_STRIDE * DISPLAY_HEIGHT,
                  SCE_GXM_COLOR_SURFACE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_RW,
                  &g_display_uid[i]);
    if (!g_display_buffer[i]) {
      fail("display buffer", (int)i);
      return 0;
    }
    memset(g_display_buffer[i], 0, 4u * DISPLAY_STRIDE * DISPLAY_HEIGHT);
    sceGxmColorSurfaceInit(&g_display_surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                           SCE_GXM_COLOR_SURFACE_LINEAR,
                           SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                           SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, DISPLAY_WIDTH,
                           DISPLAY_HEIGHT, DISPLAY_STRIDE, g_display_buffer[i]);
    sceGxmSyncObjectCreate(&g_display_sync[i]);
  }

  aligned_w = ALIGN_UP(DISPLAY_WIDTH, SCE_GXM_TILE_SIZEX);
  aligned_h = ALIGN_UP(DISPLAY_HEIGHT, SCE_GXM_TILE_SIZEY);
  sample_count = aligned_w * aligned_h;
  depth_mem = gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 4u * sample_count, 4,
                        SCE_GXM_MEMORY_ATTRIB_RW, &g_depth_uid);
  if (!depth_mem) {
    fail("depth buffer", 0);
    return 0;
  }
  sceGxmDepthStencilSurfaceInit(&g_depth_surface,
                                SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24,
                                SCE_GXM_DEPTH_STENCIL_SURFACE_TILED,
                                (int)aligned_w, depth_mem, NULL);
  /* GXM has no depth-clear call. Force-load off means each scene starts its
   * tiles at the background depth, which is the clear; force-store off means
   * the buffer is never written back to memory, since nothing outside the
   * scene reads it. */
  sceGxmDepthStencilSurfaceSetBackgroundDepth(&g_depth_surface, 1.0f);
  sceGxmDepthStencilSurfaceSetForceLoadMode(
      &g_depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_DISABLED);
  sceGxmDepthStencilSurfaceSetForceStoreMode(
      &g_depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_DISABLED);

  patch_buf = gxm_alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, 2u * 1024u * 1024u, 4,
                        SCE_GXM_MEMORY_ATTRIB_RW, &g_patch_buf_uid);
  patch_vert = usse_alloc(512u * 1024u, &g_patch_vert_uid, &patch_vert_offset, 0);
  patch_frag = usse_alloc(512u * 1024u, &g_patch_frag_uid, &patch_frag_offset, 1);
  if (!patch_buf || !patch_vert || !patch_frag) {
    fail("patcher memory", 0);
    return 0;
  }

  memset(&patch, 0, sizeof(patch));
  patch.hostAllocCallback = patcher_host_alloc;
  patch.hostFreeCallback = patcher_host_free;
  patch.bufferMem = patch_buf;
  patch.bufferMemSize = 2u * 1024u * 1024u;
  patch.vertexUsseMem = patch_vert;
  patch.vertexUsseMemSize = 512u * 1024u;
  patch.vertexUsseOffset = patch_vert_offset;
  patch.fragmentUsseMem = patch_frag;
  patch.fragmentUsseMemSize = 512u * 1024u;
  patch.fragmentUsseOffset = patch_frag_offset;
  rc = sceGxmShaderPatcherCreate(&patch, &g_patcher);
  if (rc < 0) {
    fail("sceGxmShaderPatcherCreate", rc);
    return 0;
  }

  g_back = 0;
  g_front = DISPLAY_BUFFERS - 1u;
  g_up = 1;
  return 1;
}

SceGxmContext *gxm_context(void) { return g_context; }
SceGxmShaderPatcher *gxm_patcher(void) { return g_patcher; }
unsigned gxm_width(void) { return DISPLAY_WIDTH; }
unsigned gxm_height(void) { return DISPLAY_HEIGHT; }
const char *gxm_error(void) { return g_error; }

void gxm_begin_scene(void) {
  if (!g_up)
    return;
  sceGxmBeginScene(g_context, 0, g_target, NULL, NULL, g_display_sync[g_back],
                   &g_display_surface[g_back], &g_depth_surface);
}

void gxm_end_scene(void) {
  DisplayData data;
  if (!g_up)
    return;
  sceGxmEndScene(g_context, NULL, NULL);
  sceGxmPadHeartbeat(&g_display_surface[g_back], g_display_sync[g_back]);

  data.address = g_display_buffer[g_back];
  sceGxmDisplayQueueAddEntry(g_display_sync[g_front], g_display_sync[g_back],
                             &data);
  g_front = g_back;
  g_back = (g_back + 1u) % DISPLAY_BUFFERS;
  g_frames++;
}

int gxm_capture(unsigned char *rgba) {
  const unsigned char *src;
  unsigned y;
  if (!g_up || !g_frames)
    return 0;
  gxm_finish();
  src = (const unsigned char *)g_display_buffer[g_front];
  for (y = 0; y < DISPLAY_HEIGHT; y++)
    memcpy(rgba + (size_t)y * DISPLAY_WIDTH * 4u,
           src + (size_t)y * DISPLAY_STRIDE * 4u, DISPLAY_WIDTH * 4u);
  return 1;
}

void gxm_finish(void) {
  if (!g_up)
    return;
  sceGxmFinish(g_context);
  sceGxmDisplayQueueFinish();
}

void gxm_shutdown(void) {
  unsigned i;
  if (!g_up)
    return;
  gxm_finish();
  sceGxmShaderPatcherDestroy(g_patcher);
  g_patcher = NULL;
  sceGxmDestroyRenderTarget(g_target);
  g_target = NULL;
  for (i = 0; i < DISPLAY_BUFFERS; i++) {
    gxm_free(g_display_uid[i]);
    sceGxmSyncObjectDestroy(g_display_sync[i]);
  }
  gxm_free(g_depth_uid);
  gxm_free(g_patch_buf_uid);
  usse_free(g_patch_vert_uid, 0);
  usse_free(g_patch_frag_uid, 1);
  sceGxmDestroyContext(g_context);
  g_context = NULL;
  gxm_free(g_vdm_uid);
  gxm_free(g_vertex_uid);
  gxm_free(g_fragment_uid);
  usse_free(g_frag_usse_uid, 1);
  free(g_host_mem);
  g_host_mem = NULL;
  sceGxmTerminate();
  g_up = 0;
}

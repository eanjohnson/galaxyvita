#include "gx_gxm.h"

#include "gx_texture.h"
#include "gxm.h"
#include "shader_cache.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* XF register file addresses, the same two the host rasteriser reads. */
enum {
  XF_VIEWPORT = 0x101Au,  /* wd/2, -ht/2, zrange, xorig, yorig, farz */
  XF_PROJECTION = 0x1020u /* 6 params, then the type word */
};

/* The EFB this game renders into. Both are upper bounds: a copy names its own
 * smaller rectangle, and that rectangle is what the output scale is derived
 * from once the first frame has been presented. */
enum { EFB_W = 640, EFB_H = 528 };

/* One frame's geometry. Two of them, because the GPU is still reading the
 * previous frame's arena when the next scene starts recording into this one. */
enum {
  ARENA_VERTS = 96u * 1024u,
  ARENA_INDICES = 192u * 1024u,
  ARENA_COUNT = 2
};

/* Position is GX clip space, not screen space: the GPU does the divide, so the
 * near-plane clip and perspective-correct interpolation are the hardware's.
 * Colour is U8N because that is what the vertex loader already produced. */
typedef struct {
  float x, y, z, w;
  float u, v;
  unsigned char c[4];
} GvVert;

typedef struct {
  GvVert *verts;
  unsigned int *indices;
  SceUID vert_uid, index_uid;
} Arena;

/* The passthrough pipeline. Cg, not GLSL: the console's own compiler is the
 * only one available (see docs/GRAPHICS-BACKEND.md), and it is a Cg compiler.
 *
 * The name a program is cached under has to change whenever this source does,
 * or the first run's blob would be loaded forever. */
#define GV_PASSTHROUGH_VERSION "v1"


static const char k_vertex_cg[] =
    "void main(\n"
    "    float4 aPosition : POSITION,\n"
    "    float4 aColor    : COLOR,\n"
    "    float2 aTexcoord : TEXCOORD0,\n"
    "    out float4 vPosition : POSITION,\n"
    "    out float4 vColor    : COLOR,\n"
    "    out float2 vTexcoord : TEXCOORD0)\n"
    "{\n"
    "  vPosition = aPosition;\n"
    "  vColor = aColor;\n"
    "  vTexcoord = aTexcoord;\n"
    "}\n";

static const char k_fragment_cg[] =
    "float4 main(float4 vColor : COLOR) : COLOR\n"
    "{\n"
    "  return vColor;\n"
    "}\n";

/* The textured half of the same pipeline. Modulating the sampled texel by the
 * raster colour is what tests/host/gx_soft.c does in place of TEV, and it is
 * deliberately the same approximation here so the two renderers can still be
 * diffed frame for frame. Real TEV stage translation replaces both. */
static const char k_fragment_tex_cg[] =
    "float4 main(float4 vColor : COLOR, float2 vTexcoord : TEXCOORD0,\n"
    "            uniform sampler2D sTex : TEXUNIT0) : COLOR\n"
    "{\n"
    "  return tex2D(sTex, vTexcoord) * vColor;\n"
    "}\n";

/* Decoded textures, kept for the life of the run.
 *
 * Keyed by (address, format, size) exactly as the host rasteriser keys its
 * own cache, and like the host's this never invalidates: the game re-binds the
 * same handful of images every frame and nothing in the boot rewrites one in
 * place. A texture the game does edit would go stale here, which is a real
 * limit and the reason the key includes the format and the size rather than
 * just the address.
 *
 * The pixels come out of one arena rather than a memblock each, because a Vita
 * memblock rounds up to 4 KB and the UI atlases are mostly smaller than that. */
enum { TEX_CACHE = 512, TEX_ARENA_BYTES = 24u * 1024u * 1024u };

typedef struct {
  u32 addr, fmt;
  int w, h;
  SceGxmTexture tex;
  int ok;
} GvTex;

typedef struct {
  const GxRegs *regs;

  SceGxmShaderPatcherId vertex_id, fragment_id, fragment_tex_id;
  SceGxmVertexProgram *vertex_program;
  SceGxmFragmentProgram *fragment_program;
  SceGxmFragmentProgram *fragment_tex_program;

  unsigned char *tex_arena;
  SceUID tex_arena_uid;
  unsigned tex_arena_used;
  GvTex tex[TEX_CACHE];
  unsigned tex_n;
  unsigned tex_decoded, tex_unmapped, tex_unsupported, tex_nospace;
  unsigned textured_draws;
  const GvTex *bound; /* what the context currently has on TEXUNIT0 */

  Arena arena[ARENA_COUNT];
  int arena_index;
  unsigned vert_used, index_used;

  int in_scene;
  unsigned prim_base;  /* first vertex of the primitive being assembled */
  unsigned prim_count; /* vertices accepted so far in this primitive */
  unsigned char prim;
  int prim_ok;

  /* The viewport in force for the primitive being assembled. GX rewrites it
   * between draws, so it is captured per primitive rather than per frame. */
  float vp[6];

  /* Output placement, recomputed when a copy names a new rectangle. */
  float scale, offset_x, offset_y;
  unsigned copy_w, copy_h;

  unsigned frames, draws, verts, scenes;
  unsigned dropped_full, dropped_nomatrix, dropped_novp, dropped_unsupported;

  char dump_dir[128];
  unsigned dump_count, dump_stride, dumped;
  unsigned char *dump_buf;
} GxGxm;

static GxGxm g;
static char g_error[256];

/* ------------------------------------------------------------------ */

static float xf_f(const GxRegs *f, unsigned addr) {
  union {
    unsigned u;
    float f;
  } bits;
  bits.u = addr < GX_XF_WORDS ? f->xf[addr] : 0u;
  return bits.f;
}

/* Object space -> GX clip space. The position matrix is three rows of four
 * floats in XF matrix memory and PNMTXIDX counts rows, so it starts at idx*4.
 * This is tests/host/gx_soft.c's transform(), minus the screen projection the
 * GPU now does. */
static int to_clip(const GxVertex *v, float out[4]) {
  const GxRegs *f = g.regs;
  unsigned base = (unsigned)v->pnmtx * 4u;
  float ex = 0.0f, ey = 0.0f, ez = 0.0f;
  float p0, p1, p2, p3, p4, p5;
  int r;

  if (base + 12u > 256u) {
    g.dropped_nomatrix++;
    return 0;
  }
  for (r = 0; r < 3; r++) {
    float m0 = xf_f(f, base + (unsigned)r * 4u + 0);
    float m1 = xf_f(f, base + (unsigned)r * 4u + 1);
    float m2 = xf_f(f, base + (unsigned)r * 4u + 2);
    float m3 = xf_f(f, base + (unsigned)r * 4u + 3);
    float d = m0 * v->pos[0] + m1 * v->pos[1] + m2 * v->pos[2] + m3;
    if (r == 0)
      ex = d;
    else if (r == 1)
      ey = d;
    else
      ez = d;
  }

  p0 = xf_f(f, XF_PROJECTION + 0);
  p1 = xf_f(f, XF_PROJECTION + 1);
  p2 = xf_f(f, XF_PROJECTION + 2);
  p3 = xf_f(f, XF_PROJECTION + 3);
  p4 = xf_f(f, XF_PROJECTION + 4);
  p5 = xf_f(f, XF_PROJECTION + 5);
  if (f->xf[XF_PROJECTION + 6] & 1u) {
    out[0] = p0 * ex + p1;
    out[1] = p2 * ey + p3;
    out[2] = p4 * ez + p5;
    out[3] = 1.0f;
  } else {
    out[0] = p0 * ex + p1 * ez;
    out[1] = p2 * ey + p3 * ez;
    out[2] = p4 * ez + p5;
    out[3] = -ez;
  }
  return 1;
}

/* ------------------------------------------------------------------ */

static int build_pipeline(void) {
  SceGxmShaderPatcher *patcher = gxm_patcher();
  const SceGxmProgram *vs, *fs, *fs_tex;
  SceGxmVertexAttribute attrs[3];
  SceGxmVertexStream stream;
  const SceGxmProgramParameter *p_pos, *p_col, *p_tex;
  SceGxmBlendInfo blend;
  int rc;

  vs = gvsc_program("gv_passthrough_" GV_PASSTHROUGH_VERSION ".vert",
                    k_vertex_cg, 0);
  if (!vs) {
    snprintf(g_error, sizeof(g_error), "vertex shader: %s", gvsc_last_error());
    return 0;
  }
  fs = gvsc_program("gv_passthrough_" GV_PASSTHROUGH_VERSION ".frag",
                    k_fragment_cg, 1);
  if (!fs) {
    snprintf(g_error, sizeof(g_error), "fragment shader: %s", gvsc_last_error());
    return 0;
  }

  rc = sceGxmShaderPatcherRegisterProgram(patcher, vs, &g.vertex_id);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "register vertex: 0x%08x", (unsigned)rc);
    return 0;
  }
  rc = sceGxmShaderPatcherRegisterProgram(patcher, fs, &g.fragment_id);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "register fragment: 0x%08x", (unsigned)rc);
    return 0;
  }

  p_pos = sceGxmProgramFindParameterByName(vs, "aPosition");
  p_col = sceGxmProgramFindParameterByName(vs, "aColor");
  p_tex = sceGxmProgramFindParameterByName(vs, "aTexcoord");
  if (!p_pos || !p_col || !p_tex) {
    /* The compiler drops an attribute the shader does not use, and the
     * resulting binding is silently wrong rather than an error, so this is
     * checked rather than assumed. */
    snprintf(g_error, sizeof(g_error), "vertex attributes missing: pos=%d col=%d tex=%d",
             p_pos != NULL, p_col != NULL, p_tex != NULL);
    return 0;
  }

  memset(attrs, 0, sizeof(attrs));
  attrs[0].streamIndex = 0;
  attrs[0].offset = 0;
  attrs[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attrs[0].componentCount = 4;
  attrs[0].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p_pos);
  attrs[1].streamIndex = 0;
  attrs[1].offset = 16;
  attrs[1].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
  attrs[1].componentCount = 2;
  attrs[1].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p_tex);
  attrs[2].streamIndex = 0;
  attrs[2].offset = 24;
  attrs[2].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
  attrs[2].componentCount = 4;
  attrs[2].regIndex = (uint16_t)sceGxmProgramParameterGetResourceIndex(p_col);

  stream.stride = sizeof(GvVert);
  stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_32BIT;

  rc = sceGxmShaderPatcherCreateVertexProgram(patcher, g.vertex_id, attrs, 3,
                                              &stream, 1, &g.vertex_program);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "create vertex program: 0x%08x", (unsigned)rc);
    return 0;
  }

  /* Source-alpha blending is the one piece of per-draw state kept fixed here:
   * without it the interface layers over the top of the frame would punch
   * black holes in it, which reads as a geometry bug rather than a missing
   * blend mode. The real per-draw state comes with the TEV pipeline. */
  memset(&blend, 0, sizeof(blend));
  blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
  blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
  blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
  blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
  blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
  blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;

  rc = sceGxmShaderPatcherCreateFragmentProgram(
      patcher, g.fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
      SCE_GXM_MULTISAMPLE_NONE, &blend, vs, &g.fragment_program);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "create fragment program: 0x%08x", (unsigned)rc);
    return 0;
  }

  /* The textured variant shares the vertex program, the attributes and the
   * blend state; only the fragment half differs, so it is a second program
   * against the same `vs` rather than a second pipeline. */
  fs_tex = gvsc_program("gv_passthrough_" GV_PASSTHROUGH_VERSION "_tex.frag",
                        k_fragment_tex_cg, 1);
  if (!fs_tex) {
    snprintf(g_error, sizeof(g_error), "textured fragment shader: %s",
             gvsc_last_error());
    return 0;
  }
  rc = sceGxmShaderPatcherRegisterProgram(patcher, fs_tex, &g.fragment_tex_id);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "register textured fragment: 0x%08x",
             (unsigned)rc);
    return 0;
  }
  rc = sceGxmShaderPatcherCreateFragmentProgram(
      patcher, g.fragment_tex_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
      SCE_GXM_MULTISAMPLE_NONE, &blend, vs, &g.fragment_tex_program);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "create textured fragment program: 0x%08x",
             (unsigned)rc);
    return 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ */

static unsigned gx_addr_mode(u32 gx_wrap) {
  switch (gx_wrap & 3u) {
  case 1: return SCE_GXM_TEXTURE_ADDR_REPEAT;
  case 2: return SCE_GXM_TEXTURE_ADDR_MIRROR;
  default: return SCE_GXM_TEXTURE_ADDR_CLAMP;
  }
}

/* The texture bound to map 0, decoded and uploaded on first sight.
 *
 * Map 0 only, for the same reason the host rasteriser samples map 0 only:
 * compositing several maps is TEV's job, not a wider sampler's. Returns NULL
 * whenever the draw should go out untextured, having counted why. */
static const GvTex *bind_texture(void) {
  const GxRegs *f = g.regs;
  GxTexRef ref;
  GvTex *t;
  const u8 *src;
  u32 need, ws, wt;
  unsigned bytes, i;

  if (!gx_tex_enabled(f) || !gx_tex_describe(f, 0u, &ref))
    return NULL;

  for (i = 0; i < g.tex_n; i++) {
    t = &g.tex[i];
    if (t->addr == ref.addr && t->fmt == ref.fmt && t->w == ref.w &&
        t->h == ref.h)
      return t->ok ? t : NULL;
  }
  if (g.tex_n >= TEX_CACHE)
    return NULL;

  t = &g.tex[g.tex_n++];
  t->addr = ref.addr;
  t->fmt = ref.fmt;
  t->w = ref.w;
  t->h = ref.h;
  t->ok = 0;

  need = gx_tex_size(ref.fmt, ref.w, ref.h);
  if (need == 0u) {
    g.tex_unsupported++;
    return NULL;
  }
  src = f->mem_fn ? f->mem_fn(f->mem_user, ref.addr & 0x3FFFFFFFu, need) : NULL;
  if (!src) {
    g.tex_unmapped++;
    return NULL;
  }

  /* 16-byte aligned, which is SCE_GXM_TEXTURE_ALIGNMENT; the decoded image is
   * a multiple of four bytes wide so only the base needs rounding. */
  bytes = (unsigned)ref.w * (unsigned)ref.h * 4u;
  g.tex_arena_used = (g.tex_arena_used + (SCE_GXM_TEXTURE_ALIGNMENT - 1u)) &
                     ~(unsigned)(SCE_GXM_TEXTURE_ALIGNMENT - 1u);
  if (!g.tex_arena || g.tex_arena_used + bytes > TEX_ARENA_BYTES) {
    g.tex_nospace++;
    return NULL;
  }
  {
    unsigned char *px = g.tex_arena + g.tex_arena_used;
    if (!gx_tex_decode(src, need, ref.fmt, ref.w, ref.h, px)) {
      g.tex_unsupported++;
      return NULL;
    }
    g.tex_arena_used += bytes;
    /* The decoder writes R,G,B,A in memory order, which read as a
     * little-endian word is A<<24|B<<16|G<<8|R -- ABGR. */
    if (sceGxmTextureInitLinear(&t->tex, px,
                                SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                (unsigned)ref.w, (unsigned)ref.h, 0) < 0) {
      g.tex_unsupported++;
      return NULL;
    }
  }

  gx_tex_wrap(f, &ws, &wt);
  sceGxmTextureSetUAddrMode(&t->tex, gx_addr_mode(ws));
  sceGxmTextureSetVAddrMode(&t->tex, gx_addr_mode(wt));
  /* Point sampling, to match the host rasteriser exactly.
   *
   * The hardware filtered bilinearly and this will want to as well, but the
   * host PNG of the same frame is the only oracle there is for what the
   * console should be drawing, and the first run with textures on it is the
   * wrong moment to make the two disagree on purpose. One line, once the
   * frames line up. */
  sceGxmTextureSetMinFilter(&t->tex, SCE_GXM_TEXTURE_FILTER_POINT);
  sceGxmTextureSetMagFilter(&t->tex, SCE_GXM_TEXTURE_FILTER_POINT);
  t->ok = 1;
  g.tex_decoded++;
  return t;
}

int gx_gxm_init(const GxRegs *regs) {
  int i;
  memset(&g, 0, sizeof(g));
  g_error[0] = '\0';
  g.regs = regs;
  g.tex_arena_uid = -1;
  g.copy_w = EFB_W;
  g.copy_h = 448;
  g.scale = 1.0f;

  for (i = 0; i < ARENA_COUNT; i++) {
    g.arena[i].verts = (GvVert *)gxm_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ARENA_VERTS * sizeof(GvVert), 4,
        SCE_GXM_MEMORY_ATTRIB_READ, &g.arena[i].vert_uid);
    g.arena[i].indices = (unsigned int *)gxm_alloc(
        SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, ARENA_INDICES * sizeof(unsigned int), 4,
        SCE_GXM_MEMORY_ATTRIB_READ, &g.arena[i].index_uid);
    if (!g.arena[i].verts || !g.arena[i].indices) {
      snprintf(g_error, sizeof(g_error), "geometry arena %d: %s", i, gxm_error());
      return 0;
    }
  }
  g.tex_arena = (unsigned char *)gxm_alloc(
      SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, TEX_ARENA_BYTES,
      SCE_GXM_TEXTURE_ALIGNMENT, SCE_GXM_MEMORY_ATTRIB_READ, &g.tex_arena_uid);
  if (!g.tex_arena) {
    /* Not fatal: without it every draw goes out untextured, which is exactly
     * what this backend did before textures existed. A run that reports
     * tex_nospace on every draw is more use than one that refuses to boot. */
    g.tex_arena_uid = -1;
  }
  if (!build_pipeline())
    return 0;
  /* Recomputed on the first copy; a sane default keeps the very first frames
   * from being drawn at an arbitrary scale. */
  g.scale = 544.0f / 448.0f;
  g.offset_x = (960.0f - 640.0f * g.scale) * 0.5f;
  g.offset_y = 0.0f;
  return 1;
}

void gx_gxm_shutdown(void) {
  int i;
  SceGxmShaderPatcher *patcher = gxm_patcher();
  if (patcher) {
    if (g.vertex_program)
      sceGxmShaderPatcherReleaseVertexProgram(patcher, g.vertex_program);
    if (g.fragment_program)
      sceGxmShaderPatcherReleaseFragmentProgram(patcher, g.fragment_program);
    if (g.fragment_tex_program)
      sceGxmShaderPatcherReleaseFragmentProgram(patcher, g.fragment_tex_program);
  }
  if (g.tex_arena_uid >= 0)
    gxm_free(g.tex_arena_uid);
  for (i = 0; i < ARENA_COUNT; i++) {
    gxm_free(g.arena[i].vert_uid);
    gxm_free(g.arena[i].index_uid);
  }
  memset(&g, 0, sizeof(g));
}

/* ------------------------------------------------------------------ */

static void set_viewport_from(const float vp[6]) {
  /* gx_soft.c's project(), moved onto the GPU: x = ndc*wd + (xorig-342), and
   * the 342 is the fixed bias GX bakes into the viewport origin. The scale and
   * offset put the EFB rectangle on the Vita's wider screen without stretching
   * it. */
  float x_off = (vp[3] - 342.0f) * g.scale + g.offset_x;
  float x_scale = vp[0] * g.scale;
  float y_off = (vp[4] - 342.0f) * g.scale + g.offset_y;
  float y_scale = vp[1] * g.scale; /* already negative: GX stores -ht/2 */
  sceGxmSetViewport(gxm_context(), x_off, x_scale, y_off, y_scale, vp[5], vp[2]);
}

/* The ground colour, read from the game's copy-clear registers exactly as
 * tests/host/gx_soft.c's clear_efb() reads them: BP 0x4F is alpha:red, BP 0x50
 * is green:blue.
 *
 * This used to be a hardcoded dark blue, chosen so that "the GXM path ran and
 * presented" was distinguishable from "nothing reached the screen". It did its
 * job and then became the difference between a host frame and a device frame
 * of the same moment -- the host cleared to the game's black, the console to
 * the debug blue -- which is noise in the one comparison that decides whether
 * the console is drawing the right thing. */
static void clear_colour(unsigned char out[4]) {
  const u32 ar = g.regs ? g.regs->bp[0x4Fu] : 0u;
  const u32 gb = g.regs ? g.regs->bp[0x50u] : 0u;
  out[0] = (unsigned char)(ar & 0xFFu);
  out[1] = (unsigned char)((gb >> 8) & 0xFFu);
  out[2] = (unsigned char)(gb & 0xFFu);
  out[3] = 0xFFu;
}

/* GXM has no colour-clear call either, and unlike depth there is no background
 * value to lean on -- the only way to put a known colour in every pixel is to
 * draw one. A single oversized triangle does it in one draw with no clipping
 * seam, which is why it is not a quad. */
static void clear_screen(void) {
  SceGxmContext *ctx = gxm_context();
  GvVert *v = g.arena[g.arena_index].verts;
  unsigned int *idx = g.arena[g.arena_index].indices;
  static const float k_pos[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
  unsigned char rgba[4];
  int i;

  clear_colour(rgba);
  for (i = 0; i < 3; i++) {
    v[i].x = k_pos[i][0];
    v[i].y = k_pos[i][1];
    v[i].z = 0.5f;
    v[i].w = 1.0f;
    v[i].u = 0.0f;
    v[i].v = 0.0f;
    memcpy(v[i].c, rgba, 4);
    idx[i] = (unsigned int)i;
  }
  g.vert_used = 3;
  g.index_used = 3;

  sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetBackDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_ALWAYS);
  sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
  sceGxmSetBackDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_DISABLED);
  sceGxmSetViewport(ctx, (float)gxm_width() * 0.5f, (float)gxm_width() * 0.5f,
                    (float)gxm_height() * 0.5f, (float)gxm_height() * 0.5f, 0.5f,
                    0.5f);
  sceGxmSetVertexStream(ctx, 0, v);
  sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U32, idx, 3);
}

static void begin_scene_if_needed(void) {
  SceGxmContext *ctx = gxm_context();
  if (g.in_scene)
    return;
  g.arena_index = (g.arena_index + 1) % ARENA_COUNT;
  g.vert_used = 0;
  g.index_used = 0;
  gxm_begin_scene();
  g.in_scene = 1;
  g.scenes++;

  sceGxmSetVertexProgram(ctx, g.vertex_program);
  sceGxmSetFragmentProgram(ctx, g.fragment_program);
  g.bound = NULL;
  sceGxmSetCullMode(ctx, SCE_GXM_CULL_NONE);
  clear_screen();
  sceGxmSetFrontDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_LESS_EQUAL);
  sceGxmSetBackDepthFunc(ctx, SCE_GXM_DEPTH_FUNC_LESS_EQUAL);
  sceGxmSetFrontDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
  sceGxmSetBackDepthWriteEnable(ctx, SCE_GXM_DEPTH_WRITE_ENABLED);
}

static void on_prim_begin(void *user, u8 prim, u32 count) {
  const GxRegs *f = g.regs;
  int i;
  (void)user;
  (void)count;

  begin_scene_if_needed();
  g.prim = prim;
  g.prim_base = g.vert_used;
  g.prim_count = 0;
  g.prim_ok = 1;
  for (i = 0; i < 6; i++)
    g.vp[i] = xf_f(f, XF_VIEWPORT + (unsigned)i);
  if (g.vp[0] == 0.0f) {
    /* No viewport has been loaded yet, so there is nothing to project into.
     * The host rasteriser counts the same case. */
    g.dropped_novp++;
    g.prim_ok = 0;
  }
}

static void on_vertex(void *user, const GxVertex *v) {
  GvVert *out;
  float clip[4];
  (void)user;
  if (!g.prim_ok)
    return;
  if (g.vert_used >= ARENA_VERTS) {
    g.dropped_full++;
    g.prim_ok = 0;
    return;
  }
  if (!to_clip(v, clip)) {
    g.prim_ok = 0;
    return;
  }
  out = &g.arena[g.arena_index].verts[g.vert_used++];
  out->x = clip[0];
  out->y = clip[1];
  out->z = clip[2];
  out->w = clip[3];
  out->u = v->has_tex0 ? v->tex0[0] : 0.0f;
  out->v = v->has_tex0 ? v->tex0[1] : 0.0f;
  if (v->has_color0) {
    memcpy(out->c, v->color0, 4);
  } else {
    out->c[0] = out->c[1] = out->c[2] = out->c[3] = 0xFF;
  }
  g.prim_count++;
}

static int push_index(unsigned value) {
  if (g.index_used >= ARENA_INDICES) {
    g.dropped_full++;
    return 0;
  }
  g.arena[g.arena_index].indices[g.index_used++] = value;
  return 1;
}

static int push_triangle(unsigned a, unsigned b, unsigned c) {
  return push_index(a) && push_index(b) && push_index(c);
}

/* GX primitive -> a triangle list. Doing the topology here rather than mapping
 * onto GXM's strip and fan types keeps one draw path, and the strip's
 * alternating winding is handled the same way the host rasteriser handles it:
 * the cull mode is off, so winding only has to be consistent, not correct. */
static unsigned emit_topology(unsigned char prim, unsigned base, unsigned n) {
  unsigned start = g.index_used;
  unsigned i;
  switch (prim & 0xF8u) {
  case 0x80: /* quads */
  case 0x88: /* quads, second vertex order */
    for (i = 0; i + 3u < n; i += 4u) {
      if (!push_triangle(base + i, base + i + 1u, base + i + 2u))
        break;
      if (!push_triangle(base + i, base + i + 2u, base + i + 3u))
        break;
    }
    break;
  case 0x90: /* triangles */
    for (i = 0; i + 2u < n; i += 3u)
      if (!push_triangle(base + i, base + i + 1u, base + i + 2u))
        break;
    break;
  case 0x98: /* triangle strip */
    for (i = 0; i + 2u < n; i++) {
      if (i & 1u) {
        if (!push_triangle(base + i + 1u, base + i, base + i + 2u))
          break;
      } else {
        if (!push_triangle(base + i, base + i + 1u, base + i + 2u))
          break;
      }
    }
    break;
  case 0xA0: /* triangle fan */
    for (i = 1u; i + 1u < n; i++)
      if (!push_triangle(base, base + i, base + i + 1u))
        break;
    break;
  default:
    /* Lines and points reach the GPU as their own primitive types, which
     * means their own draw call and their own state. They are a rounding
     * error in this game's frames, so they are counted and skipped rather
     * than half-supported. */
    g.dropped_unsupported++;
    break;
  }
  return g.index_used - start;
}

static void on_prim_end(void *user) {
  SceGxmContext *ctx = gxm_context();
  unsigned first = g.index_used;
  unsigned count;
  (void)user;

  if (!g.prim_ok || g.prim_count < 3u)
    return;
  count = emit_topology(g.prim, g.prim_base, g.prim_count);
  if (count == 0u)
    return;

  /* Which program this draw goes out on depends on the texture the BP
   * registers name right now, so it is decided per primitive -- GX rebinds
   * between draws as freely as it rewrites the viewport. Both are only
   * pushed to the context when they actually change. */
  {
    const GvTex *tex = bind_texture();
    if (tex != g.bound) {
      sceGxmSetFragmentProgram(ctx, tex ? g.fragment_tex_program
                                        : g.fragment_program);
      g.bound = tex;
    }
    if (tex) {
      sceGxmSetFragmentTexture(ctx, 0, &tex->tex);
      g.textured_draws++;
    }
  }

  set_viewport_from(g.vp);
  sceGxmSetVertexStream(ctx, 0, g.arena[g.arena_index].verts);
  sceGxmDraw(ctx, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U32,
             &g.arena[g.arena_index].indices[first], count);
  g.draws++;
  g.verts += g.prim_count;
}

static void dump_frame(void) {
  char path[192];
  SceUID fd;
  if (!g.dump_dir[0] || g.dumped >= g.dump_count)
    return;
  if (g.dump_stride && (g.frames % g.dump_stride) != 0u)
    return;
  if (!g.dump_buf) {
    g.dump_buf = (unsigned char *)malloc((size_t)gxm_width() * gxm_height() * 4u);
    if (!g.dump_buf) {
      g.dump_dir[0] = '\0';
      return;
    }
  }
  if (!gxm_capture(g.dump_buf))
    return;
  snprintf(path, sizeof(path), "%s/frame_%04u.raw", g.dump_dir, g.frames);
  fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return;
  sceIoWrite(fd, g.dump_buf, (SceSize)(gxm_width() * gxm_height() * 4u));
  sceIoClose(fd);
  g.dumped++;
}

void gx_gxm_dump_frames(const char *dir, unsigned count, unsigned stride) {
  snprintf(g.dump_dir, sizeof(g.dump_dir), "%s", dir);
  sceIoMkdir(g.dump_dir, 0777);
  g.dump_count = count;
  g.dump_stride = stride ? stride : 1u;
}

static void present(void) {
  if (!g.in_scene)
    return;
  gxm_end_scene();
  /* Stage 1 runs the whole pipeline on the emulated CPU's thread and waits for
   * the GPU before reusing an arena. Emulation is far slower than the display
   * here, so the wait costs nothing measurable and it removes a whole class of
   * "the GPU was still reading that" bug from the bring-up. */
  gxm_finish();
  g.in_scene = 0;
  dump_frame();
  g.frames++;
}

static void on_copy(void *user, u32 data) {
  const GxRegs *f = g.regs;
  (void)user;
  /* Bit 14 is copy-to-XFB: a finished frame. Everything else is a
   * render-to-texture copy and must not be mistaken for one. */
  if (!((data >> 14) & 1u))
    return;

  {
    unsigned wh = f->bp[0x4A];
    unsigned w = (wh & 0x3FFu) + 1u;
    unsigned h = ((wh >> 10) & 0x3FFu) + 1u;
    if (w > 0u && w <= EFB_W && h > 0u && h <= EFB_H &&
        (w != g.copy_w || h != g.copy_h)) {
      float sx = (float)gxm_width() / (float)w;
      float sy = (float)gxm_height() / (float)h;
      g.copy_w = w;
      g.copy_h = h;
      /* Uniform scale with the remainder split evenly: the Vita's panel is
       * 16:9 and the game's frame is 4:3, so filling the width would stretch
       * it. Pillarbox instead. */
      g.scale = sx < sy ? sx : sy;
      g.offset_x = ((float)gxm_width() - (float)w * g.scale) * 0.5f;
      g.offset_y = ((float)gxm_height() - (float)h * g.scale) * 0.5f;
    }
  }
  present();
}

void gx_gxm_sink(GxSink *out) {
  memset(out, 0, sizeof(*out));
  out->user = &g;
  out->prim_begin = on_prim_begin;
  out->vertex = on_vertex;
  out->prim_end = on_prim_end;
  out->copy = on_copy;
}

const char *gx_gxm_error(void) { return g_error; }

void gx_gxm_report(char *out, unsigned n) {
  snprintf(out, n,
           "gxm frames=%u scenes=%u draws=%u vtx=%u drop_full=%u "
           "drop_nomtx=%u drop_novp=%u drop_prim=%u scale=%.3f copy=%ux%u "
           "dumped=%u tex=%u/%u arena=%uk miss_unmap=%u miss_fmt=%u "
           "miss_space=%u",
           g.frames, g.scenes, g.draws, g.verts, g.dropped_full,
           g.dropped_nomatrix, g.dropped_novp, g.dropped_unsupported,
           (double)g.scale, g.copy_w, g.copy_h, g.dumped, g.textured_draws,
           g.tex_decoded, g.tex_arena_used / 1024u, g.tex_unmapped,
           g.tex_unsupported, g.tex_nospace);
}

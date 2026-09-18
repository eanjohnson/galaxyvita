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
#define GV_PASSTHROUGH_VERSION "v2"


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

/* Alpha test, BP 0xF3, as arithmetic rather than as shader variants.
 *
 * GX compares the fragment's alpha against two references and combines the
 * two answers with a logic op: 8 comparisons each and 4 ops, which as compiled
 * variants would be hundreds of shaders -- and ShaccCg wedges after five to
 * nine compiles in one process, so a variant per state is not something this
 * can afford. Instead each comparison is a dot product over (a<ref, a==ref,
 * a>ref, 1) with a weight vector the CPU picks, and the logic op is a second
 * weighted sum over (1, r0, r1, r0*r1). The uniforms carry the state, one
 * shader covers every combination, and -- deliberately -- all-zero uniforms
 * mean "kill nothing", so a failed upload degrades to the old behaviour
 * rather than to an empty frame. */
static const char k_fragment_cg[] =
    "float4 main(float4 vColor : COLOR,\n"
"            uniform float4 uAlphaRef,\n"
"            uniform float4 uAlphaW0,\n"
"            uniform float4 uAlphaW1,\n"
"            uniform float4 uAlphaOp) : COLOR\n"
    "{\n"
    "  float4 c = vColor;\n"
"  float a = c.w;\n"
"  float e = uAlphaRef.z;\n"
"  float l0 = step(e, uAlphaRef.x - a);\n"
"  float g0 = step(e, a - uAlphaRef.x);\n"
"  float q0 = 1.0 - max(l0, g0);\n"
"  float r0 = uAlphaW0.x * l0 + uAlphaW0.y * q0 + uAlphaW0.z * g0\n"
"           + uAlphaW0.w;\n"
"  float l1 = step(e, uAlphaRef.y - a);\n"
"  float g1 = step(e, a - uAlphaRef.y);\n"
"  float q1 = 1.0 - max(l1, g1);\n"
"  float r1 = uAlphaW1.x * l1 + uAlphaW1.y * q1 + uAlphaW1.z * g1\n"
"           + uAlphaW1.w;\n"
"  float k = uAlphaOp.x + uAlphaOp.y * r0 + uAlphaOp.z * r1\n"
"          + uAlphaOp.w * r0 * r1;\n"
"  if (k > 0.5) discard;\n"
    "  return c;\n"
    "}\n";

/* The textured half of the same pipeline. Modulating the sampled texel by the
 * raster colour is what tests/host/gx_soft.c does in place of TEV, and it is
 * deliberately the same approximation here so the two renderers can still be
 * diffed frame for frame. Real TEV stage translation replaces both. */
/* Debug variants of the textured fragment shader, chosen by fragdbg.txt.
 *
 * A device round trip is a 26 MB build, a 35 MB upload and several minutes of
 * guest time before the first interesting frame, so testing one hypothesis per
 * rebuild is the expensive way to do this. These are the three outputs that
 * between them say where a white rectangle comes from: 1 shows the texcoord
 * varying as red/green, so a flat frame means the attribute never reached the
 * GPU and a gradient means it did; 2 drops the modulate, separating a bad
 * sample from a raster colour that is already white. Each needs its own cache
 * name or the first variant compiled would be reloaded for every later one. */
static const char k_fragment_uv_cg[] =
    "float4 main(float2 vTexcoord : TEXCOORD0) : COLOR\n"
    "{\n"
    "  return float4(vTexcoord.x, vTexcoord.y, 0.0, 1.0);\n"
    "}\n";

static const char k_fragment_texonly_cg[] =
    "float4 main(float2 vTexcoord : TEXCOORD0,\n"
    "            uniform sampler2D sTex : TEXUNIT0) : COLOR\n"
    "{\n"
    "  return tex2D(sTex, vTexcoord);\n"
    "}\n";

static const char k_fragment_tex_cg[] =
    "float4 main(float4 vColor : COLOR, float2 vTexcoord : TEXCOORD0,\n"
    "            uniform sampler2D sTex : TEXUNIT0,\n"
"            uniform float4 uAlphaRef,\n"
"            uniform float4 uAlphaW0,\n"
"            uniform float4 uAlphaW1,\n"
"            uniform float4 uAlphaOp) : COLOR\n"
    "{\n"
    "  float4 c = tex2D(sTex, vTexcoord) * vColor;\n"
"  float a = c.w;\n"
"  float e = uAlphaRef.z;\n"
"  float l0 = step(e, uAlphaRef.x - a);\n"
"  float g0 = step(e, a - uAlphaRef.x);\n"
"  float q0 = 1.0 - max(l0, g0);\n"
"  float r0 = uAlphaW0.x * l0 + uAlphaW0.y * q0 + uAlphaW0.z * g0\n"
"           + uAlphaW0.w;\n"
"  float l1 = step(e, uAlphaRef.y - a);\n"
"  float g1 = step(e, a - uAlphaRef.y);\n"
"  float q1 = 1.0 - max(l1, g1);\n"
"  float r1 = uAlphaW1.x * l1 + uAlphaW1.y * q1 + uAlphaW1.z * g1\n"
"           + uAlphaW1.w;\n"
"  float k = uAlphaOp.x + uAlphaOp.y * r0 + uAlphaOp.z * r1\n"
"          + uAlphaOp.w * r0 * r1;\n"
"  if (k > 0.5) discard;\n"
    "  return c;\n"
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
/* Distinct blend states seen in a run. GX has 8x8 factor pairs times enable
 * times colour-update times textured, but a frame uses a handful; anything
 * past this falls back to the fixed program rather than failing the draw. */
enum { FRAG_VARIANTS = 64 };

typedef struct {
  unsigned key;
  SceGxmFragmentProgram *prog;
} GvFragProg;

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
  const SceGxmProgram *vs; /* the vertex program the fragment halves match */
  /* One fragment program per blend state, built on demand. The blend mode is
   * baked into a GXM fragment program, so per-draw blending means per-draw
   * program selection; they are all patched from the same two compiled
   * shaders, so none of this costs a ShaccCg compile. */
  GvFragProg frag[FRAG_VARIANTS];
  unsigned frag_n;
  int frag_rc;
  SceGxmFragmentProgram *prog_bound;
  /* uAlpha* parameters, [0] untextured and [1] textured. */
  const SceGxmProgramParameter *u_ref[2], *u_w0[2], *u_w1[2], *u_op[2];
  unsigned depth_func, depth_write;
  int depth_valid;
  /* The GX state the last draw of the frame went out under, so a frame that
   * comes back wrong says which register produced it. */
  u32 last_bmode, last_zmode, last_amode;
  unsigned draws_blend, draws_nocolor, draws_atest;

  unsigned char *tex_arena;
  SceUID tex_arena_uid;
  unsigned tex_arena_used;
  GvTex tex[TEX_CACHE];
  unsigned tex_n;
  unsigned tex_decoded, tex_unmapped, tex_unsupported, tex_nospace;
  unsigned textured_draws;
  unsigned sampler_index;
  unsigned frag_mode;
  /* The bind's own return code, and what was bound. sceGxmSetFragmentTexture
   * reports failure and the call site threw it away, so a rejected texture
   * left the unit unbound -- which samples white, which is the symptom. */
  int set_tex_rc;
  int validate_rc;
  int init_rc;
  unsigned tex_init_mode;
  int last_tex_w, last_tex_h;
  /* The texture the final draw of a frame used, sampled when the frame is
   * copied out, and the draws of each frame counted by the size of what they
   * sampled. Between them they say which draw ends up on top, which is the
   * question a frame dump cannot answer on its own. */
  int final_tex_w, final_tex_h;
  unsigned draws_tex_tiny, draws_tex_big, draws_untex;
  /* The range of texture coordinates that actually arrived. A
   * correct texture that every fragment samples at one point is
   * indistinguishable from a broken sampler in a frame dump, and
   * the strap image's (0,0) texel is white -- so the two candidate
   * faults produce the same white rectangle. This separates them. */
  float uv_lo[2], uv_hi[2];
  unsigned uv_n;
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
  unsigned snap_done, snaps;
  unsigned char *dump_buf;
} GxGxm;

static GxGxm g;
static char g_error[256];
/* Outside `g`, because gx_gxm_init() memsets it and the mode has to survive
 * being chosen before the backend comes up. */
static unsigned g_frag_mode;

void gx_gxm_set_frag_mode(unsigned mode) { g_frag_mode = mode; }

/* How a decoded image is turned into an SceGxmTexture. Every stage of the
 * texture path has been measured correct on the console and tex2D still
 * returns white, so the remaining candidates are all in this one call: the
 * swizzle convention for U8U8U8U8, whether the implicit stride is what the
 * hardware wants, and whether a linear texture is accepted here at all.
 * Each device run now costs a reboot's worth of risk, so they are selectable
 * and a single run can try all of them rather than one per upload. */
static unsigned g_tex_init_mode;

void gx_gxm_set_tex_init_mode(unsigned mode) { g_tex_init_mode = mode; }


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
  const SceGxmProgramParameter *p_pos, *p_col, *p_tex, *p_smp;
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

  g.vs = vs;
  rc = sceGxmShaderPatcherCreateVertexProgram(patcher, g.vertex_id, attrs, 3,
                                              &stream, 1, &g.vertex_program);
  if (rc < 0) {
    snprintf(g_error, sizeof(g_error), "create vertex program: 0x%08x", (unsigned)rc);
    return 0;
  }

  /* Source-alpha blending, as the starting state only. Every draw picks its
   * own program out of frag_program() below; these two exist so that the
   * clear has something bound before the first draw chooses, and as the
   * fallback if the variant cache ever fills. */
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
  {
    static const char *const k_name[3] = {
        "gv_passthrough_" GV_PASSTHROUGH_VERSION "_tex.frag",
        "gv_passthrough_" GV_PASSTHROUGH_VERSION "_uv.frag",
        "gv_passthrough_" GV_PASSTHROUGH_VERSION "_texonly.frag"};
    static const char *const k_src[3] = {k_fragment_tex_cg, k_fragment_uv_cg,
                                         k_fragment_texonly_cg};
    const unsigned m = g.frag_mode < 3u ? g.frag_mode : 0u;
    fs_tex = gvsc_program(k_name[m], k_src[m], 1);
  }
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

  /* Which texture unit the sampler actually landed on.
   *
   * The attributes above are all bound by asking the compiled program where it
   * put them, with a comment saying why guessing is unsafe -- and then the
   * sampler was bound to unit 0 by assumption anyway. TEXUNIT0 in the source is
   * a request, not a guarantee, and a wrong unit binds the texture somewhere
   * nothing samples: every fragment reads the default, which is white, and the
   * frame comes out as a white rectangle that looks exactly like no texturing
   * at all. */
  p_smp = sceGxmProgramFindParameterByName(fs_tex, "sTex");
  if (!p_smp && g.frag_mode != 1u) {
    snprintf(g_error, sizeof(g_error), "sampler sTex missing from program");
    return 0;
  }
  g.sampler_index =
      p_smp ? (unsigned)sceGxmProgramParameterGetResourceIndex(p_smp) : 0u;

  /* The alpha-test uniforms, looked up rather than assumed: a debug fragment
   * variant does not carry them at all, and a missing one simply means the
   * test is not applied to that program instead of an error. */
  {
    const SceGxmProgram *const prog[2] = {fs, fs_tex};
    unsigned i;
    for (i = 0; i < 2u; i++) {
      g.u_ref[i] = sceGxmProgramFindParameterByName(prog[i], "uAlphaRef");
      g.u_w0[i] = sceGxmProgramFindParameterByName(prog[i], "uAlphaW0");
      g.u_w1[i] = sceGxmProgramFindParameterByName(prog[i], "uAlphaW1");
      g.u_op[i] = sceGxmProgramFindParameterByName(prog[i], "uAlphaOp");
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Per-draw GX state: BLENDMODE (BP 0x41), ZMODE (BP 0x40), alpha test
 * (BP 0xF3). tests/host/gx_soft.c already learned why these matter -- its
 * own comment says it outright: "Without these a 2D overlay drawn last paints
 * over the whole frame." That is exactly the white rectangle this backend
 * draws, so the state the host reads is the state this reads. */

/* BP 0x41 bits 8-10, the source factor. 2 and 3 name the destination there. */
static unsigned gx_src_factor(u32 sel) {
  switch (sel & 7u) {
  case 0: return SCE_GXM_BLEND_FACTOR_ZERO;
  case 1: return SCE_GXM_BLEND_FACTOR_ONE;
  case 2: return SCE_GXM_BLEND_FACTOR_DST_COLOR;
  case 3: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
  case 4: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
  case 5: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case 6: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
  default: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
}

/* BP 0x41 bits 5-7, the destination factor. 2 and 3 name the source there. */
static unsigned gx_dst_factor(u32 sel) {
  switch (sel & 7u) {
  case 0: return SCE_GXM_BLEND_FACTOR_ZERO;
  case 1: return SCE_GXM_BLEND_FACTOR_ONE;
  case 2: return SCE_GXM_BLEND_FACTOR_SRC_COLOR;
  case 3: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  case 4: return SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
  case 5: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  case 6: return SCE_GXM_BLEND_FACTOR_DST_ALPHA;
  default: return SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
}

static SceGxmFragmentProgram *frag_program(int textured, u32 bmode) {
  SceGxmFragmentProgram *const fallback =
      textured ? g.fragment_tex_program : g.fragment_program;
  const unsigned en = bmode & 1u;
  const unsigned cup = (bmode >> 3) & 1u;
  const unsigned dfac = (bmode >> 5) & 7u;
  const unsigned sfac = (bmode >> 8) & 7u;
  const unsigned key = en | (cup << 1) | (dfac << 2) | (sfac << 5) |
                       ((unsigned)(textured != 0) << 8);
  SceGxmBlendInfo blend;
  unsigned i;
  int rc;

  for (i = 0; i < g.frag_n; i++)
    if (g.frag[i].key == key)
      return g.frag[i].prog;
  if (g.frag_n >= FRAG_VARIANTS || !g.vs)
    return fallback;

  memset(&blend, 0, sizeof(blend));
  /* Alpha is deliberately not written. The host rasteriser leaves the frame's
   * alpha at 0xFF whatever a draw does, and the clear here writes 0xFF too, so
   * masking alpha off keeps the two frame dumps comparable byte for byte
   * instead of leaving the console's alpha wherever the last draw put it. */
  blend.colorMask = cup ? (SceGxmColorMask)(SCE_GXM_COLOR_MASK_R |
                                            SCE_GXM_COLOR_MASK_G |
                                            SCE_GXM_COLOR_MASK_B)
                        : SCE_GXM_COLOR_MASK_NONE;
  blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
  blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
  blend.colorSrc = en ? (SceGxmBlendFactor)gx_src_factor(sfac)
                      : SCE_GXM_BLEND_FACTOR_ONE;
  blend.colorDst = en ? (SceGxmBlendFactor)gx_dst_factor(dfac)
                      : SCE_GXM_BLEND_FACTOR_ZERO;
  blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
  blend.alphaDst = SCE_GXM_BLEND_FACTOR_ONE;

  rc = sceGxmShaderPatcherCreateFragmentProgram(
      gxm_patcher(), textured ? g.fragment_tex_id : g.fragment_id,
      SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE, &blend,
      g.vs, &g.frag[g.frag_n].prog);
  if (rc < 0) {
    g.frag_rc = rc;
    return fallback;
  }
  g.frag[g.frag_n].key = key;
  return g.frag[g.frag_n++].prog;
}

/* One GX alpha comparison as weights over (a < ref, a == ref, a > ref, 1). */
static void alpha_weights(u32 comp, float *w) {
  w[0] = w[1] = w[2] = w[3] = 0.0f;
  switch (comp & 7u) {
  case 0: break;                                /* never */
  case 1: w[0] = 1.0f; break;                   /* less */
  case 2: w[1] = 1.0f; break;                   /* equal */
  case 3: w[0] = 1.0f; w[1] = 1.0f; break;      /* lequal */
  case 4: w[2] = 1.0f; break;                   /* greater */
  case 5: w[0] = 1.0f; w[2] = 1.0f; break;      /* nequal */
  case 6: w[1] = 1.0f; w[2] = 1.0f; break;      /* gequal */
  default: w[3] = 1.0f; break;                  /* always */
  }
}

/* BP 0xF3 for "compare always, and always, keep everything". The default
 * uniform buffer GXM hands out is not cleared, so a draw that never uploads
 * its own alpha state tests against whatever the last scene left in that
 * memory -- which killed the clear itself and left the frame showing the
 * previous contents of the buffer. Every program that can discard uploads a
 * state before it is first used. */
#define GX_ALPHA_ALWAYS 0x003F0000u

static void set_alpha_uniforms_reg(int textured, u32 reg) {
  SceGxmContext *const ctx = gxm_context();
  const int i = textured ? 1 : 0;
  float ref[4], w0[4], w1[4], op[4];
  void *buf = NULL;

  if (!g.u_ref[i] || !g.u_w0[i] || !g.u_w1[i] || !g.u_op[i])
    return;
  ref[0] = (float)(reg & 0xFFu) * (1.0f / 255.0f);
  ref[1] = (float)((reg >> 8) & 0xFFu) * (1.0f / 255.0f);
  ref[2] = 0.5f / 255.0f; /* half a step, so "equal" survives interpolation */
  ref[3] = 0.0f;
  alpha_weights((reg >> 16) & 7u, w0);
  alpha_weights((reg >> 19) & 7u, w1);
  /* The op weights are written for the kill, not the pass, so that uniforms
   * that never arrived leave every fragment alive. */
  switch ((reg >> 22) & 3u) {
  case 0: op[0] = 1.0f; op[1] = 0.0f; op[2] = 0.0f; op[3] = -1.0f; break;
  case 1: op[0] = 1.0f; op[1] = -1.0f; op[2] = -1.0f; op[3] = 1.0f; break;
  case 2: op[0] = 1.0f; op[1] = -1.0f; op[2] = -1.0f; op[3] = 2.0f; break;
  default: op[0] = 0.0f; op[1] = 1.0f; op[2] = 1.0f; op[3] = -2.0f; break;
  }
  if (sceGxmReserveFragmentDefaultUniformBuffer(ctx, &buf) < 0 || !buf)
    return;
  sceGxmSetUniformDataF(buf, g.u_ref[i], 0, 4, ref);
  sceGxmSetUniformDataF(buf, g.u_w0[i], 0, 4, w0);
  sceGxmSetUniformDataF(buf, g.u_w1[i], 0, 4, w1);
  sceGxmSetUniformDataF(buf, g.u_op[i], 0, 4, op);
}

static void set_alpha_uniforms(int textured) {
  set_alpha_uniforms_reg(textured, g.regs ? g.regs->bp[0xF3u] : 0u);
}

static void set_depth_state(u32 zmode) {
  SceGxmContext *const ctx = gxm_context();
  const unsigned test = zmode & 1u;
  const unsigned write = (zmode >> 4) & 1u;
  unsigned func;

  switch (test ? ((zmode >> 1) & 7u) : 7u) {
  case 0: func = SCE_GXM_DEPTH_FUNC_NEVER; break;
  case 1: func = SCE_GXM_DEPTH_FUNC_LESS; break;
  case 2: func = SCE_GXM_DEPTH_FUNC_EQUAL; break;
  case 3: func = SCE_GXM_DEPTH_FUNC_LESS_EQUAL; break;
  case 4: func = SCE_GXM_DEPTH_FUNC_GREATER; break;
  case 5: func = SCE_GXM_DEPTH_FUNC_NOT_EQUAL; break;
  case 6: func = SCE_GXM_DEPTH_FUNC_GREATER_EQUAL; break;
  default: func = SCE_GXM_DEPTH_FUNC_ALWAYS; break;
  }
  if (!g.depth_valid || func != g.depth_func) {
    sceGxmSetFrontDepthFunc(ctx, (SceGxmDepthFunc)func);
    sceGxmSetBackDepthFunc(ctx, (SceGxmDepthFunc)func);
    g.depth_func = func;
  }
  if (!g.depth_valid || write != g.depth_write) {
    const SceGxmDepthWriteMode w =
        write ? SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
    sceGxmSetFrontDepthWriteEnable(ctx, w);
    sceGxmSetBackDepthWriteEnable(ctx, w);
    g.depth_write = write;
  }
  g.depth_valid = 1;
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
    /* Frag mode 3 overwrites the decode with a synthetic gradient.
     *
     * Everything measurable about this texture already reads correct -- the
     * decode is byte-perfect when it is pulled back off the card, the
     * texcoords span the quad, the sampler resolves to unit zero and GXM
     * validates the texture -- and yet the quad samples solid white. That
     * leaves two possibilities that no amount of reporting separates: the
     * bytes the GPU reads are not the bytes the CPU wrote, or the bytes the
     * CPU wrote are not the ones this code believes it wrote. Writing a
     * pattern that cannot come from the game, through the same allocation and
     * the same texture object, tells the two apart in one run. */
    if (g.frag_mode == 3u) {
      int yy, xx;
      const int dw = ref.w > 1 ? ref.w - 1 : 1;
      const int dh = ref.h > 1 ? ref.h - 1 : 1;
      for (yy = 0; yy < ref.h; yy++) {
        for (xx = 0; xx < ref.w; xx++) {
          unsigned char *q = px + ((unsigned)yy * (unsigned)ref.w +
                                   (unsigned)xx) * 4u;
          q[0] = (unsigned char)(xx * 255 / dw);
          q[1] = (unsigned char)(yy * 255 / dh);
          q[2] = 0x80u;
          q[3] = 0xFFu;
        }
      }
    }
    g.tex_arena_used += bytes;
    /* The decoder writes R,G,B,A in memory order, which read as a
     * little-endian word is A<<24|B<<16|G<<8|R -- ABGR. */
    {
      int irc;
      switch (g.tex_init_mode) {
      case 1: /* the other swizzle, in case the naming convention is inverted */
        irc = sceGxmTextureInitLinear(&t->tex, px,
                                      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_RGBA,
                                      (unsigned)ref.w, (unsigned)ref.h, 0);
        break;
      case 2: /* an explicit byte stride rather than the implicit one */
        irc = sceGxmTextureInitLinearStrided(
            &t->tex, px, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
            (unsigned)ref.w, (unsigned)ref.h, (unsigned)ref.w * 4u);
        break;
      default:
        irc = sceGxmTextureInitLinear(&t->tex, px,
                                      SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                                      (unsigned)ref.w, (unsigned)ref.h, 0);
        break;
      }
      g.init_rc = irc;
      if (irc < 0) {
        g.tex_unsupported++;
        return NULL;
      }
    }
    /* GXM will tell us outright whether this texture is usable, which is worth
     * more than any amount of reasoning about alignment and stride rules: a
     * texture that initialises without error can still be rejected at draw
     * time, and a rejected texture samples as white. */
    g.validate_rc = sceGxmTextureValidate(&t->tex);
  }

  /* The decoded image, straight to the card the first few times.
   *
   * "The screen is white" has two causes that look identical from the host: a
   * texture that decoded to white, and a correct texture that nothing samples.
   * The frame dump cannot tell them apart and a device round trip is thirty-
   * five megabytes and ten minutes, so the run that asks the question should
   * carry the answer to both halves. */
  if (g.dump_dir[0] && g.tex_decoded < 4u) {
    char path[192];
    SceUID fd;
    snprintf(path, sizeof(path), "%s/tex_%02u_%dx%d_f%u.raw", g.dump_dir,
             g.tex_decoded, ref.w, ref.h, (unsigned)ref.fmt);
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd >= 0) {
      sceIoWrite(fd, g.tex_arena + (g.tex_arena_used - bytes), (SceSize)bytes);
      sceIoClose(fd);
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
  g.frag_mode = g_frag_mode;
  g.tex_init_mode = g_tex_init_mode;
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
  /* The clear below moves the depth state, and the per-draw state is only
   * pushed when it changes, so both caches start each scene empty. */
  g.prog_bound = g.fragment_program;
  g.depth_valid = 0;
  set_alpha_uniforms_reg(0, GX_ALPHA_ALWAYS);
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
  /* Unconditionally, as tests/host/gx_soft.c does. has_tex0 is set for every
   * vertex the loader produces, so the ternary never chose the fallback; it
   * only made the two renderers look like they disagreed. */
  out->u = v->tex0[0];
  out->v = v->tex0[1];
  if (g.uv_n == 0u) {
    g.uv_lo[0] = g.uv_hi[0] = out->u;
    g.uv_lo[1] = g.uv_hi[1] = out->v;
  } else {
    if (out->u < g.uv_lo[0]) g.uv_lo[0] = out->u;
    if (out->u > g.uv_hi[0]) g.uv_hi[0] = out->u;
    if (out->v < g.uv_lo[1]) g.uv_lo[1] = out->v;
    if (out->v > g.uv_hi[1]) g.uv_hi[1] = out->v;
  }
  g.uv_n++;
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
    const u32 bmode = g.regs ? g.regs->bp[0x41u] : 0u;
    const u32 zmode = g.regs ? g.regs->bp[0x40u] : 0u;
    SceGxmFragmentProgram *const prog = frag_program(tex != NULL, bmode);
    if (prog != g.prog_bound) {
      sceGxmSetFragmentProgram(ctx, prog);
      g.prog_bound = prog;
    }
    g.bound = tex;
    set_alpha_uniforms(tex != NULL);
    set_depth_state(zmode);
    g.last_bmode = bmode;
    g.last_zmode = zmode;
    g.last_amode = g.regs ? g.regs->bp[0xF3u] : 0u;
    if (bmode & 1u)
      g.draws_blend++;
    if (!((bmode >> 3) & 1u))
      g.draws_nocolor++;
    if (((g.last_amode >> 16) & 7u) != 7u || ((g.last_amode >> 19) & 7u) != 7u)
      g.draws_atest++;
    if (!tex)
      g.draws_untex++;
    else if (tex->w <= 16 && tex->h <= 16)
      g.draws_tex_tiny++;
    else
      g.draws_tex_big++;
    if (tex) {
      const int rc = sceGxmSetFragmentTexture(ctx, g.sampler_index, &tex->tex);
      if (rc < 0)
        g.set_tex_rc = rc;
      g.last_tex_w = tex->w;
      g.last_tex_h = tex->h;
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

/* Outside `g`, for the same reason the fragment mode is: the request comes
 * from the watchdog thread, and gx_gxm_init() memsets everything in `g`. */
static volatile unsigned g_snap_req;

void gx_gxm_snap(unsigned serial) { g_snap_req = serial; }

/* One frame, on demand, to a fixed name.
 *
 * The dump window is fixed when the backend comes up, which is the wrong
 * shape for driving the game: a run that has just been sent a button press
 * needs the frame after the press, and which frame that is cannot be known in
 * advance. A serial written to the card from the host asks for the next one. */
static void snap_frame(void) {
  char path[192];
  SceUID fd;
  const unsigned want = g_snap_req;
  if (want == g.snap_done || !g.dump_dir[0])
    return;
  g.snap_done = want;
  if (!g.dump_buf) {
    g.dump_buf =
        (unsigned char *)malloc((size_t)gxm_width() * gxm_height() * 4u);
    if (!g.dump_buf)
      return;
  }
  if (!gxm_capture(g.dump_buf))
    return;
  snprintf(path, sizeof(path), "%s/snap.raw", g.dump_dir);
  fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0)
    return;
  sceIoWrite(fd, g.dump_buf, (SceSize)(gxm_width() * gxm_height() * 4u));
  sceIoClose(fd);
  g.snaps++;
}

static void dump_frame(void) {
  char path[192];
  SceUID fd;
  snap_frame();
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

  /* Whatever was bound last is what the topmost draw of this frame used. */
  g.final_tex_w = g.last_tex_w;
  g.final_tex_h = g.last_tex_h;

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

void gx_gxm_flush_textures(void) {
  /* Forget every decoded texture so the next bind re-creates it under whatever
   * init mode is now selected. The caller must have waited for the GPU first:
   * the arena is reused from the start, and a scene still in flight is still
   * reading it. Only the selftest needs this -- the game never changes mode
   * mid-run. */
  g.tex_n = 0;
  g.tex_arena_used = 0;
  g.tex_decoded = 0;
  g.bound = NULL;
  g.prog_bound = NULL;
  memset(g.tex, 0, sizeof(g.tex));
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
           "dumped=%u snaps=%u tex=%u/%u arena=%uk miss_unmap=%u miss_fmt=%u "
           "miss_space=%u smp=%u fdbg=%u last=%dx%d fin=%dx%d dtiny=%u "
           "dbig=%u duntex=%u bm=%08x zm=%08x am=%08x fvar=%u frc=0x%08x "
           "dblend=%u dnocol=%u datest=%u uv=[%.3f %.3f]-[%.3f %.3f]",
           g.frames, g.scenes, g.draws, g.verts, g.dropped_full,
           g.dropped_nomatrix, g.dropped_novp, g.dropped_unsupported,
           (double)g.scale, g.copy_w, g.copy_h, g.dumped, g.snaps,
           g.textured_draws,
           g.tex_decoded, g.tex_arena_used / 1024u, g.tex_unmapped,
           g.tex_unsupported, g.tex_nospace, g.sampler_index,
           g.frag_mode, g.last_tex_w, g.last_tex_h, g.final_tex_w,
           g.final_tex_h, g.draws_tex_tiny, g.draws_tex_big, g.draws_untex,
           (unsigned)g.last_bmode, (unsigned)g.last_zmode,
           (unsigned)g.last_amode, g.frag_n, (unsigned)g.frag_rc,
           g.draws_blend, g.draws_nocolor, g.draws_atest,
           (double)g.uv_lo[0], (double)g.uv_lo[1], (double)g.uv_hi[0],
           (double)g.uv_hi[1]);
}

#!/usr/bin/env python3
"""Rewrites a Dolphin-generated GLSL shader into Cg for the Vita's ShaccCg.

Dolphin's shader bodies are already written in an HLSL-flavoured dialect --
float4, int4, iround -- which the OpenGL backend aliases down to vec4/ivec4 in
its preamble. Cg wants the original spelling back, so most of this file is
undoing that. What is genuinely structural is the I/O: GLSL declares varyings as
globals, Cg passes them through the entry point with semantics. That is handled
by hoisting each declaration to a `static` global and generating an entry point
that copies in, calls the body, and copies out -- which keeps the generated body
untouched.

Usage: glsl_to_cg.py <in.vert|in.frag> <out.cg>
"""
import re
import sys

# Dolphin emits these; Cg spells them differently or not at all.
# The Vita's Cg profile has no unsigned integer type at all: `uint` and `uint4`
# are not keywords, and every one of Dolphin's packed BP uniforms and bitfield
# helpers trips on them. Signed is the only width-correct option available.
# Shifts are the one place this is not a pure renaming -- see fix_shifts.
ALIASES = [
    # GLSL `matCxR` is C columns by R rows; Cg `floatRxC` is R rows by C
    # columns. The two spellings land on the same storage here because
    # Dolphin only ever assigns whole 4-vectors by index and then multiplies
    # row-vector-first (`v * M`), which maps to `mul(M, v)` -- the transpose
    # cancels on both sides. Verified against the only two call sites:
    # dolphin_position_matrix (mat3x4) and dolphin_normal_matrix (mat3).
    (r'\bmat3x4\b', 'float3x4'),
    (r'\bmat4x3\b', 'float4x3'),
    (r'\bmat3\b', 'float3x3'),
    (r'\bmat4\b', 'float4x4'),
    (r'\bvec([234])\b', r'float\1'),
    (r'\bivec([234])\b', r'int\1'),
    (r'\buvec([234])\b', r'int\1'),
    (r'\buint([234])\b', r'int\1'),
    (r'\buint\b', 'int'),
    (r'\bfract\b', 'frac'),
    (r'\bmix\b', 'lerp'),
    (r'\bdFdx\b', 'ddx'),
    (r'\bdFdy\b', 'ddy'),
    (r'\bmod\b', 'fmod'),
    # ES has this built in from 3.1; Dolphin emits a fallback when the backend
    # says it does not. Cg has neither, so keep the fallback under its own name.
    (r'\bbitfieldExtract\b', 'gv_bfe'),
]

# GXM has no 2D array textures. Dolphin only uses the array layer for stereo,
# which is off, so the layer coordinate is always zero and the type collapses.
COLLAPSE_ARRAY = [
    (r'\bsampler2DArray\b', 'sampler2D'),
    (r'\btexture\s*\(\s*tex\s*,\s*coords\s*,\s*lod_bias\s*\)',
     'tex2Dlod(tex, float4(coords.xy, 0.0, lod_bias))'),
    (r'float3 coords = float3\(([^;]+), layer\);',
     r'float3 coords = float3(\1, 0.0);'),
]


UBO = re.compile(r'UBO_BINDING\(std140,\s*\d+\)\s*uniform\s+\w+\s*\{(.*?)\n\};',
                 re.S)


def flatten_uniform_blocks(src):
    """Turns each std140 block into top-level uniforms.

    Cg has no anonymous uniform block, and the generated body refers to every
    member unqualified, so the members have to become uniforms in their own
    right. GXM offers 14 uniform buffers and Dolphin wants three, so nothing is
    lost by putting them in the default one -- the packing is the backend's
    problem either way, since it binds by reflected parameter rather than by
    offset.
    """
    def one(m):
        out = []
        for line in m.group(1).split('\n'):
            body = line.strip()
            # Dolphin puts its accessor macros *inside* the block --
            # `#define xfmem_color(i) (xfmem_pack1[(i)].z)`. A preprocessor
            # line is not a member and must pass through untouched; prefixing
            # it produces `uniform #define`, which stops the parser dead.
            if not body or body.startswith('//') or body.startswith('#'):
                out.append(line)
                continue
            out.append('uniform ' + body)
        return '\n'.join(out)
    return UBO.sub(one, src)



# Dolphin emits two overloads of bitfieldExtract, one over uint and one over
# int. With uint gone they collide, so the unsigned one goes; every call site
# lands on the signed version once the types are rewritten.
UINT_BFE = re.compile(
    r'uint bitfieldExtract\(uint val, int off, int size\) \{.*?\n\}\n',
    re.S)


def drop_uint_bfe(src):
    return UINT_BFE.sub('', src)


# `>>` on a uint is a logical shift; on an int it is arithmetic, so a set sign
# bit smears instead of bringing in zeros. Dolphin's own fallback masks after
# shifting, which recovers the low bits, but the extracted width must not
# include bit 31. Keep the mask explicit so the intent survives review.
def fix_shifts(src):
    return src.replace(
        'return int(val >> off) & mask;',
        'return (val >> off) & mask;  /* mask discards the smeared sign */')


DECL = re.compile(
    r'^\s*(?:ATTRIBUTE_LOCATION|VARYING_LOCATION|FRAGMENT_OUTPUT_LOCATION_INDEXED'
    r'|FRAGMENT_OUTPUT_LOCATION)\(([^)]*)\)\s*(in|out)\s+(\S+)\s+(\w+)\s*;',
    re.M)

# Cg semantics. Attributes and varyings are matched positionally by index, which
# is what SceGxmProgram reflection then binds against.
def semantic(stage, direction, name, index):
    if stage == 'vert' and direction == 'in':
        return f'TEXCOORD{index}'
    return f'TEXCOORD{index}'


# GLSL lets an array size sit on the type -- `DolphinLightData[8] lights;` --
# and Dolphin's uniform-block writer uses that form for its light and material
# arrays. Cg only accepts the size on the declarator, so move the brackets
# across. Dolphin never emits the sibling `Type[N](...)` array-constructor
# syntax (checked across all 64 generated shaders), so a declaration is the
# only thing this can match.
PREFIX_ARRAY = re.compile(
    r'\b([A-Za-z_]\w*)\[(\d+)\]\s+([A-Za-z_]\w*)\s*;')


def fix_prefix_arrays(src):
    return PREFIX_ARRAY.sub(r'\1 \3[\2];', src)


# Cg's standard library has no integer clamp, so `clamp(lacc, 0, 255)` over an
# int is ambiguous between the fixed/half/float overloads. Supplying exact
# integer matches resolves it without touching the call sites, which matters:
# the TEV code clamps intermediate values whose signed wrap-around IS the
# hardware behaviour, so rounding them through float would be a silent
# correctness bug rather than a compile error.
INT_CLAMP = """int  gv_clamp(int  x, int  a, int  b) { return x < a ? a : (x > b ? b : x); }
int2 clamp(int2 x, int2 a, int2 b) { return int2(gv_clamp(x.x,a.x,b.x), gv_clamp(x.y,a.y,b.y)); }
int3 clamp(int3 x, int3 a, int3 b) { return int3(gv_clamp(x.x,a.x,b.x), gv_clamp(x.y,a.y,b.y), gv_clamp(x.z,a.z,b.z)); }
int4 clamp(int4 x, int4 a, int4 b) { return int4(gv_clamp(x.x,a.x,b.x), gv_clamp(x.y,a.y,b.y), gv_clamp(x.z,a.z,b.z), gv_clamp(x.w,a.w,b.w)); }
int  clamp(int  x, int a, int b) { return gv_clamp(x, a, b); }
int2 clamp(int2 x, int a, int b) { return clamp(x, int2(a,a), int2(b,b)); }
int3 clamp(int3 x, int a, int b) { return clamp(x, int3(a,a,a), int3(b,b,b)); }
int4 clamp(int4 x, int a, int b) { return clamp(x, int4(a,a,a,a), int4(b,b,b,b)); }
"""

# GLSL multiplies a vector by a matrix with `*`; Cg requires mul(). Dolphin
# emits exactly one shape of this, `v * dolphin_*_matrix()`, with the vector on
# the left -- a row-vector product, which is mul(M, v) in Cg. See the matrix
# alias note above for why the transpose cancels.
VEC_TIMES_MAT = re.compile(
    r'([A-Za-z_][\w.]*)\s*\*\s*(dolphin_\w+_matrix\(\))')


def fix_matrix_mul(src):
    return VEC_TIMES_MAT.sub(r'mul(\2, \1)', src)


# Two leftovers from the unsigned rewrite, both of which Cg takes seriously:
#
#   * `0u` literals survive the type rename. Cg has no unsigned literal, and
#     the suffix is meaningless once the type is int.
#   * A file-scope `const` in Cg is a *uniform with a default value*, not a
#     compile-time constant -- the profile warns it cannot honour the default,
#     which means the value would read back as 0 at runtime. `static const` is
#     the spelling that actually produces a constant.
UNSIGNED_LITERAL = re.compile(r'\b(\d+)[uU]\b')
FILE_SCOPE_CONST = re.compile(r'^const\s', re.M)


def fix_constants(src):
    src = UNSIGNED_LITERAL.sub(r'\1', src)
    return FILE_SCOPE_CONST.sub('static const ', src)


def convert(src, stage):
    decls = []
    for m in DECL.finditer(src):
        decls.append((m.group(2), m.group(3), m.group(4)))
    # Replace each declaration with a plain mutable global the body can use.
    src = DECL.sub(lambda m: f'static {m.group(3)} {m.group(4)};', src)

    # Must precede the alias pass: once uint becomes int the two overloads are
    # textually identical and there is no way to tell which to remove.
    src = drop_uint_bfe(src)

    for pat, rep in ALIASES + COLLAPSE_ARRAY:
        src = re.sub(pat, rep, src)
    src = fix_shifts(src)
    src = fix_prefix_arrays(src)
    src = fix_matrix_mul(src)
    src = fix_constants(src)

    src = flatten_uniform_blocks(src)
    src = re.sub(r'SAMPLER_BINDING\(\d+\)\s*', '', src)
    src = re.sub(r'^\s*FORCE_EARLY_Z;\s*$', '', src, flags=re.M)

    src = src.replace('void main()', 'void gv_body()')

    ins = [d for d in decls if d[0] == 'in']
    outs = [d for d in decls if d[0] == 'out']
    params, pre, post = [], [], []

    if stage == 'vert':
        for i, (_, ty, nm) in enumerate(ins):
            params.append(f'{ty} in_{nm} : TEXCOORD{i}')
            pre.append(f'  {nm} = in_{nm};')
        params.append('out float4 gv_position : POSITION')
        post.append('  gv_position = gl_Position;')
        src = src.replace('gl_Position', 'gv_gl_Position')
        src = 'static float4 gv_gl_Position;\n' + src
        post = ['  gv_position = gv_gl_Position;']
        for i, (_, ty, nm) in enumerate(outs):
            params.append(f'out {ty} out_{nm} : TEXCOORD{i}')
            post.append(f'  out_{nm} = {nm};')
    else:
        params.append('float4 gv_wpos : WPOS')
        pre.append('  gv_gl_FragCoord = gv_wpos;')
        src = src.replace('gl_FragCoord', 'gv_gl_FragCoord')
        src = 'static float4 gv_gl_FragCoord;\n' + src
        for i, (_, ty, nm) in enumerate(ins):
            params.append(f'{ty} in_{nm} : TEXCOORD{i}')
            pre.append(f'  {nm} = in_{nm};')
        # Only ocol0 survives: dual-source blend is off on this hardware.
        params.append('out float4 gv_color : COLOR')
        post.append(f'  gv_color = {outs[0][2]};' if outs else '  gv_color = 0;')
        if 'gl_FragDepth' in src:
            src = src.replace('gl_FragDepth', 'gv_gl_FragDepth')
            src = 'static float gv_gl_FragDepth;\n' + src
            params.append('out float gv_depth : DEPTH')
            post.append('  gv_depth = gv_gl_FragDepth;')

    entry = ['', 'void main(' + (',\n          '.join(params)) + ')', '{']
    entry += pre + ['  gv_body();'] + post + ['}', '']
    return INT_CLAMP + src + '\n'.join(entry)


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: glsl_to_cg.py <in.vert|in.frag> <out.cg>')
    path, out = sys.argv[1], sys.argv[2]
    stage = 'vert' if path.endswith('.vert') else 'frag'
    with open(path) as f:
        src = f.read()
    with open(out, 'w') as f:
        f.write(convert(src, stage))


if __name__ == '__main__':
    main()

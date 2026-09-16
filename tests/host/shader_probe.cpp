// Replays captured SMG draw states into Dolphin's shader generators and writes
// the generated source out, so the Vita shader target can be judged against
// what the game actually asks for rather than against a synthetic state.
//
// Input: the blobs GV_GFX_STATEDUMP writes (see tests/host/gx_soft.c).
// Output: one .vert/.frag pair per state, plus a summary on stdout.

#include "moderngekko/dolphin_shader_compiler.hpp"

#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace moderngekko;

namespace
{
struct State
{
  std::vector<std::uint32_t> cp, xf, bp;
  std::uint8_t prim = 0;
};

bool Load(const fs::path& path, State& out)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::uint32_t hdr[6];
  in.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
  if (!in || hdr[0] != 0x54535647u || hdr[1] != 1u)
    return false;
  out.prim = static_cast<std::uint8_t>(hdr[2]);
  auto read = [&](std::vector<std::uint32_t>& v, std::uint32_t n) {
    v.resize(n);
    in.read(reinterpret_cast<char*>(v.data()), n * sizeof(std::uint32_t));
  };
  read(out.cp, hdr[3]);
  read(out.xf, hdr[4]);
  read(out.bp, hdr[5]);
  return static_cast<bool>(in);
}

// The Vita is an SGX543MP4+ behind sceGxm: GLES2-class, no geometry stage, no
// dual-source blend, no framebuffer fetch, no SSBOs. These are the flags that
// make Dolphin emit its conservative variants.
DolphinShaderCapabilities VitaCaps()
{
  DolphinShaderCapabilities c;
  c.dual_source_blend = false;
  c.geometry_shaders = false;
  c.geometry_shader_instancing = false;
  c.primitive_restart = false;
  c.early_z = false;
  c.binding_layout = false;
  c.bounding_box = false;
  c.clip_control = false;
  c.ssaa = false;
  c.fragment_stores_and_atomics = false;
  c.depth_clamp = false;
  c.reversed_depth_range = false;
  c.bitfield = false;
  c.dynamic_sampler_indexing = false;
  c.framebuffer_fetch = false;
  c.logic_op = false;
  c.palette_conversion = false;
  c.lod_bias_in_sampler = false;
  c.dynamic_vertex_loader = false;
  c.vertex_line_point_expand = false;
  c.gl_layer_in_fragment_shader = false;
  c.texture_query_levels = false;
  c.coarse_derivatives = false;
  return c;
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 3)
  {
    std::fprintf(stderr, "usage: shader_probe <state-dir> <out-dir>\n");
    return 2;
  }
  const fs::path in_dir = argv[1], out_dir = argv[2];
  fs::create_directories(out_dir);

  std::vector<fs::path> inputs;
  for (const auto& e : fs::directory_iterator(in_dir))
    if (e.path().extension() == ".bin")
      inputs.push_back(e.path());
  std::sort(inputs.begin(), inputs.end());

  const auto caps = VitaCaps();
  DolphinShaderOptions options;
  options.fast_depth = true;
  options.pixel_lighting = false;

  std::map<std::uint64_t, std::string> unique_pixel;
  std::size_t ok = 0;
  std::size_t largest = 0;
  std::string largest_name;

  for (const auto& path : inputs)
  {
    State st;
    if (!Load(path, st))
    {
      std::fprintf(stderr, "skip %s\n", path.filename().c_str());
      continue;
    }
    const GxStateView view{st.cp, st.xf, st.bp};
    const auto bundle = DolphinShaderCompiler::Compile(
        view, GxTopology::Triangles, static_cast<std::uint8_t>(st.prim & 7u),
        DolphinShaderApi::OpenGl, caps, options);

    const std::string stem = path.stem().string();
    std::ofstream(out_dir / (stem + ".vert")) << bundle.vertex;
    std::ofstream(out_dir / (stem + ".frag")) << bundle.pixel;
    unique_pixel.emplace(bundle.pixel_uid, stem);
    if (bundle.pixel.size() > largest)
    {
      largest = bundle.pixel.size();
      largest_name = stem;
    }
    std::printf("%s  vs=%zu B  ps=%zu B  ps_uid=%016llx\n", stem.c_str(),
                bundle.vertex.size(), bundle.pixel.size(),
                static_cast<unsigned long long>(bundle.pixel_uid));
    ++ok;
  }

  std::printf("\n%zu states, %zu distinct pixel UIDs, largest ps %zu B (%s)\n", ok,
              unique_pixel.size(), largest, largest_name.c_str());
  return ok ? 0 : 1;
}

/* Command processor, on top of ModernGekko.
 *
 * This replaces the hand-written FIFO parser that used to live in gx_fifo.c.
 * That parser did its job -- it got the stream decoded far enough to prove the
 * geometry was right -- but every register, every attribute format and every
 * vertex layout it understood had to be rediscovered by hand, and the census
 * and desync machinery it grew existed only to find out where it was wrong.
 *
 * ModernGekko carries Dolphin's version of all of it:
 *
 *   GxCommandProcessor  FIFO parse, display lists, indexed XF
 *   GxStateBackend      CP/XF/BP register files, vertex decode
 *   GxVertexLoader      every attribute format, direct and indexed
 *
 * so the only thing left to write here is the glue: a GpuBackend that watches
 * the register writes the Flipper cares about, and a GxRenderDevice that hands
 * decoded geometry to the runtime's C sink.
 */

#include "gx_cp.h"

#include "moderngekko/address_space.hpp"
#include "moderngekko/gx_command_processor.hpp"
#include "moderngekko/gx_state_backend.hpp"
#include "moderngekko/gx_vertex_loader.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>

namespace
{
/* BP registers the Pixel Engine turns into interrupts. */
constexpr std::uint8_t kBpSetDrawDone = 0x45;
constexpr std::uint8_t kBpTokenValue = 0x47;
constexpr std::uint8_t kBpTokenInterrupt = 0x48;
constexpr std::uint8_t kBpCopyExecute = 0x52;
/* The trace captures from the start of the stream, so it has to be big
 * enough to still be recording where the decode goes wrong. SMG's first
 * unknown opcode is at stream byte 4456810, which 1 MiB never reached --
 * the capture ran out four megabytes before the interesting part. Host
 * only, and a one-off allocation. */
constexpr std::uint32_t kTraceBytes = 16u << 20;

/* The sink speaks raw GX opcodes because the software rasteriser assembles
 * strips, fans and quads itself. GxCommandProcessor decodes the opcode to an
 * enum with `(command & 0x78) >> 3`, so this is exactly its inverse. */
std::uint8_t OpcodeFor(moderngekko::GxPrimitive primitive)
{
  return static_cast<std::uint8_t>(0x80u | (static_cast<std::uint8_t>(primitive) << 3));
}
}

/* The two roles are separate objects rather than two bases of one.
 *
 * GpuBackend and GxRenderDevice both declare InvalidateVertexCache(), so a
 * single class deriving from both has one override satisfying both interfaces:
 * the command processor's call lands in it, forwards to GxStateBackend, which
 * notifies its device -- the same object, the same function. That is infinite
 * recursion, and at -O2 it is tail-called into a silent spin rather than a
 * stack overflow, which is indistinguishable from a very slow boot.
 */
struct GxCp;

namespace
{
struct CpBackend final : public moderngekko::GpuBackend
{
  GxCp *owner = nullptr;
  void LoadCpRegister(std::uint8_t, std::uint32_t) override;
  void LoadXfRegisters(std::uint16_t, std::span<const std::uint32_t>) override;
  void LoadBpRegister(std::uint8_t, std::uint32_t) override;
  void LoadIndexedXf(std::uint8_t, std::uint16_t, std::uint16_t, std::uint8_t) override;
  void Draw(const moderngekko::GxDrawPacket &) override;
  void CallDisplayList(std::uint32_t, std::uint32_t) override;
  void InvalidateVertexCache() override;
};

struct CpDevice final : public moderngekko::GxRenderDevice
{
  GxCp *owner = nullptr;
  void SubmitDraw(const moderngekko::GxDrawPacket &,
                  const moderngekko::GxStateView &) override;
  void SubmitDecodedDraw(const moderngekko::GxDrawPacket &,
                         const moderngekko::GxDecodedDraw &,
                         const moderngekko::GxStateView &) override;
};
}

struct GxCp final
{
  explicit GxCp(std::span<std::uint8_t> mem1, std::span<std::uint8_t> mem2)
      : memory(mem1, mem2), state(memory), processor(&backend, &memory)
  {
    backend.owner = this;
    device.owner = this;
    state.SetRenderDevice(&device);
    std::memset(&regs, 0, sizeof(regs));
    regs.mem_fn = &ResolveThunk;
    regs.mem_user = this;
  }

  /* Texture fetch for the renderer, through the same AddressSpace the vertex
   * loader uses, so there is one notion of what guest memory is reachable. */
  static const u8 *ResolveThunk(void *user, u32 phys, u32 len)
  {
    auto *self = static_cast<GxCp *>(user);
    return self->memory.Resolve(phys, len);
  }

  /* --- GpuBackend: a pass-through in front of GxStateBackend. ------------
   * GxStateBackend is final, so observing its inputs means sitting in front of
   * it rather than deriving from it. Everything is forwarded unchanged; the
   * only additions are the register mirror the rasteriser reads and the PE
   * events the Flipper drains. */

  /* --- last-commands ring, for pinning a decode desync. ------------------
   * An unknown opcode says where the parse stopped agreeing with the game but
   * not why. Why is almost always a command a few back whose length we got
   * wrong -- a vertex-format register write, most often -- so keep the tail of
   * the decode and print it alongside the desync. Costs one store per command
   * and nothing else; the ring is fixed-size and never allocates. */
  struct CmdNote
  {
    std::uint64_t at;
    char kind;
    std::uint32_t a;
    std::uint32_t b;
  };
  static constexpr std::uint32_t kNotes = 512;
  CmdNote notes[kNotes]{};
  std::uint64_t note_n = 0;

  void Note(char kind, std::uint32_t a, std::uint32_t b)
  {
    notes[note_n++ % kNotes] = CmdNote{bytes_in, kind, a, b};
  }

  void DumpNotes()
  {
    std::fprintf(stderr, "  gx vcd lo=0x%08x hi=0x%08x\n", regs.vcd_low,
                 regs.vcd_high);
    for (int i = 0; i < 8; ++i)
      std::fprintf(stderr, "  gx vat[%d] = 0x%08x 0x%08x 0x%08x\n", i,
                   regs.vat[i][0], regs.vat[i][1], regs.vat[i][2]);
    const std::uint64_t first = note_n > kNotes ? note_n - kNotes : 0;
    for (std::uint64_t i = first; i < note_n; ++i)
    {
      const CmdNote &n = notes[i % kNotes];
      std::fprintf(stderr, "  gx cmd[%llu] at=%llu %c a=0x%08x b=0x%08x\n",
                   (unsigned long long)i, (unsigned long long)n.at, n.kind, n.a, n.b);
    }
    std::fflush(stderr);
  }

  void LoadCpRegister(std::uint8_t command, std::uint32_t value)
  {
    Note('C', command, value);
    switch (command & 0xF0u)
    {
    case 0x50: regs.vcd_low = value; break;
    case 0x60: regs.vcd_high = value; break;
    case 0x70: regs.vat[command & 7u][0] = value; break;
    case 0x80: regs.vat[command & 7u][1] = value; break;
    case 0x90: regs.vat[command & 7u][2] = value; break;
    default: break;
    }
    state.LoadCpRegister(command, value);
  }

  void LoadXfRegisters(std::uint16_t address,
                       std::span<const std::uint32_t> values)
  {
    Note('X', address, static_cast<std::uint32_t>(values.size()));
    state.LoadXfRegisters(address, values);
    MirrorXf(address, values.size());
  }

  void LoadBpRegister(std::uint8_t command, std::uint32_t value)
  {
    Note('B', command, value);
    state.LoadBpRegister(command, value);
    /* Read the stored value back rather than mirroring the raw one: BP
     * register 0xFE is a write mask that makes the *next* BP write partial,
     * and GxStateBackend is where that is applied. Keeping a second copy of
     * that rule here would be a second place for it to be wrong. */
    {
      const moderngekko::GxStateView view = state.GetState();
      if (command < view.bp.size())
        regs.bp[command] = view.bp[command];
    }

    switch (command)
    {
    case kBpSetDrawDone:
      pe_finish = true;
      break;
    case kBpTokenValue:
      pe_token = value & 0xFFFFu;
      pe_token_set = true;
      break;
    case kBpTokenInterrupt:
      pe_token = value & 0xFFFFu;
      pe_token_set = true;
      pe_token_irq = true;
      break;
    case kBpCopyExecute:
      /* Passed on raw: the rasteriser reads the copy-to-XFB and clear bits out
       * of it directly, and re-deriving them from a decoded struct would be a
       * second place for that decode to be wrong. */
      if (sink.copy != nullptr)
        sink.copy(sink.user, value & 0xFFFFFFu);
      break;
    default:
      break;
    }
  }

  void LoadIndexedXf(std::uint8_t array, std::uint16_t index, std::uint16_t address,
                     std::uint8_t size)
  {
    Note('I', (static_cast<std::uint32_t>(array) << 24) | index,
         (static_cast<std::uint32_t>(address) << 8) | size);
    state.LoadIndexedXf(array, index, address, size);
    /* The mirror has to see indexed loads too: they are how the projection and
     * position matrices arrive, and the rasteriser reads those out of xf[]. */
    MirrorXf(address, size);
  }

  void MirrorXf(std::uint16_t address, std::size_t count)
  {
    const moderngekko::GxStateView view = state.GetState();
    for (std::size_t i = 0; i < count; ++i)
    {
      const std::size_t slot = static_cast<std::size_t>(address) + i;
      if (slot < GX_XF_WORDS && slot < view.xf.size())
        regs.xf[slot] = view.xf[slot];
    }
  }

  void Draw(const moderngekko::GxDrawPacket& packet)
  {
    ++draws;
    last_opcode = OpcodeFor(packet.primitive);
    Note('D', OpcodeFor(packet.primitive), packet.vertex_count);
    state.Draw(packet);
  }

  void CallDisplayList(std::uint32_t address, std::uint32_t size)
  {
    /* Notification only -- GxCommandProcessor resolves and walks the list
     * itself. Counted because a list that fails to resolve is executed as
     * nothing at all, which is indistinguishable from geometry the game never
     * submitted. */
    Note('L', address, size);
    ++dls;
    dl_bytes += size;
    if (memory.Resolve(address, size) == nullptr)
    {
      ++dls_unresolved;
      if (dls_unresolved <= 8)
        std::fprintf(stderr, "gx dl unresolved addr=0x%08x size=%u\n", address, size);
    }
  }

  /* From the command processor. Forwarding into GxStateBackend is what makes
   * it notify the render device -- which is CpDevice, a different object, so
   * this terminates. */
  void InvalidateVertexCache()
  {
    Note('V', 0, 0);
    state.InvalidateVertexCache();
  }

  /* --- GxRenderDevice: decoded geometry out. ---------------------------- */

  void SubmitDraw(const moderngekko::GxDrawPacket& packet)
  {
    /* Reached only when the vertex loader could not decode the packet. Count
     * it rather than drop it silently -- a draw that vanishes here looks
     * exactly like a draw the game never issued. */
    undecoded += packet.vertex_count;
  }

  void SubmitDecodedDraw(const moderngekko::GxDrawPacket& packet,
                         const moderngekko::GxDecodedDraw& decoded)
  {
    if (sink.prim_begin == nullptr)
    {
      vertices += decoded.vertices.size();
      return;
    }

    const std::uint8_t opcode = OpcodeFor(packet.primitive);
    sink.prim_begin(sink.user, opcode, packet.vertex_count);
    /* decoded.vertices is in decode order -- the loader appends as it reads and
     * builds `indices` separately -- so streaming it reproduces the wire order
     * the rasteriser assembles from. */
    for (const moderngekko::GxVertex& v : decoded.vertices)
    {
      GxVertex out;
      std::memset(&out, 0, sizeof(out));
      out.pos[0] = v.position[0];
      out.pos[1] = v.position[1];
      out.pos[2] = v.position[2];
      out.nrm[0] = v.normal[0];
      out.nrm[1] = v.normal[1];
      out.nrm[2] = v.normal[2];
      out.tex0[0] = v.texcoord[0][0];
      out.tex0[1] = v.texcoord[0][1];
      /* GxVertexLoader stores colour as 0xRRGGBBAA. */
      out.color0[0] = static_cast<u8>((v.color[0] >> 24) & 0xFFu);
      out.color0[1] = static_cast<u8>((v.color[0] >> 16) & 0xFFu);
      out.color0[2] = static_cast<u8>((v.color[0] >> 8) & 0xFFu);
      out.color0[3] = static_cast<u8>(v.color[0] & 0xFFu);
      out.pnmtx = v.position_matrix;
      out.tex0mtx = v.texture_matrix[0];
      out.has_color0 = 1;
      out.has_tex0 = 1;
      sink.vertex(sink.user, &out);
      ++vertices;
    }
    sink.prim_end(sink.user);
  }

  CpBackend backend;
  CpDevice device;
  moderngekko::AddressSpace memory;
  moderngekko::GxStateBackend state;
  moderngekko::GxCommandProcessor processor;

  GxRegs regs{};
  GxSink sink{};

  std::uint64_t bytes_in = 0;
  std::uint64_t vertices = 0;
  std::uint64_t draws = 0;
  std::uint64_t undecoded = 0;
  std::uint8_t last_opcode = 0;

  bool pe_finish = false;
  bool pe_token_set = false;
  bool desync_reported = false;
  std::uint64_t dls = 0;
  std::uint64_t dl_bytes = 0;
  std::uint64_t dls_unresolved = 0;
  std::uint8_t *trace = nullptr;
  const char *trace_path = nullptr;
  std::uint32_t traced = 0;
  std::uint64_t writes = 0;
  bool pe_token_irq = false;
  std::uint32_t pe_token = 0;
};

namespace
{
void CpBackend::LoadCpRegister(std::uint8_t c, std::uint32_t v) { owner->LoadCpRegister(c, v); }
void CpBackend::LoadXfRegisters(std::uint16_t a, std::span<const std::uint32_t> v)
{
  owner->LoadXfRegisters(a, v);
}
void CpBackend::LoadBpRegister(std::uint8_t c, std::uint32_t v) { owner->LoadBpRegister(c, v); }
void CpBackend::LoadIndexedXf(std::uint8_t a, std::uint16_t i, std::uint16_t ad, std::uint8_t s)
{
  owner->LoadIndexedXf(a, i, ad, s);
}
void CpBackend::Draw(const moderngekko::GxDrawPacket &p) { owner->Draw(p); }
void CpBackend::CallDisplayList(std::uint32_t a, std::uint32_t s) { owner->CallDisplayList(a, s); }
void CpBackend::InvalidateVertexCache() { owner->InvalidateVertexCache(); }

void CpDevice::SubmitDraw(const moderngekko::GxDrawPacket &p, const moderngekko::GxStateView &)
{
  owner->SubmitDraw(p);
}
void CpDevice::SubmitDecodedDraw(const moderngekko::GxDrawPacket &p,
                                 const moderngekko::GxDecodedDraw &d,
                                 const moderngekko::GxStateView &)
{
  owner->SubmitDecodedDraw(p, d);
}
}

extern "C" {

GxCp *gx_cp_create(void *mem1, u32 mem1_size, void *mem2, u32 mem2_size)
{
  if (mem1 == nullptr || mem1_size == 0u)
    return nullptr;
  return new (std::nothrow)
      GxCp(std::span<std::uint8_t>(static_cast<std::uint8_t *>(mem1), mem1_size),
           std::span<std::uint8_t>(static_cast<std::uint8_t *>(mem2),
                                   mem2 != nullptr ? mem2_size : 0u));
}

void gx_cp_destroy(GxCp *cp) { delete cp; }

void gx_cp_reset(GxCp *cp)
{
  if (cp == nullptr)
    return;
  cp->processor.Reset();
  cp->state.Reset();
  std::memset(&cp->regs, 0, sizeof(cp->regs));
  cp->bytes_in = 0;
  cp->vertices = 0;
  cp->draws = 0;
  cp->undecoded = 0;
  cp->pe_finish = false;
  cp->pe_token_set = false;
  cp->pe_token_irq = false;
  cp->pe_token = 0;
}

void gx_cp_write(GxCp *cp, u64 value, u8 size)
{
  if (cp == nullptr)
    return;
  cp->bytes_in += size;
  if (cp->trace != nullptr && cp->traced < kTraceBytes)
  {
    for (std::uint8_t i = 0; i < size && cp->traced < kTraceBytes; ++i)
      cp->trace[cp->traced++] =
          static_cast<std::uint8_t>(value >> ((size - i - 1u) * 8u));
  }
  ++cp->writes;
  const std::uint64_t unknown_before = cp->processor.GetUnknownOpcodeCount();
  cp->processor.Write(value, size);
  /* Pin the first desync to a byte offset in the stream. An unknown opcode is
   * never benign here: the CP consumes one byte and resyncs on whatever
   * follows, so the first one marks where the decode stopped agreeing with the
   * game and every count after it is suspect. */
  if (cp->processor.GetUnknownOpcodeCount() != unknown_before && !cp->desync_reported)
  {
    cp->desync_reported = true;
    std::fprintf(stderr,
                 "gx first unknown opcode: stream_byte=%llu write=%llu size=%u "
                 "value=0x%016llx pending=%u cmds=%llu\n",
                 (unsigned long long)cp->bytes_in, (unsigned long long)cp->writes, size,
                 (unsigned long long)value,
                 static_cast<unsigned>(cp->processor.GetBufferedByteCount()),
                 (unsigned long long)cp->processor.GetCommandCount());
    std::fflush(stderr);
    cp->DumpNotes();
  }
}

u64 gx_cp_display_lists(const GxCp *cp) { return cp ? cp->dls : 0; }
u64 gx_cp_display_list_bytes(const GxCp *cp) { return cp ? cp->dl_bytes : 0; }
u64 gx_cp_display_lists_unresolved(const GxCp *cp) { return cp ? cp->dls_unresolved : 0; }
u64 gx_cp_undecoded(const GxCp *cp) { return cp ? cp->undecoded : 0; }

void gx_cp_trace(GxCp *cp, const char *path)
{
  if (cp == nullptr)
    return;
  cp->trace_path = path;
  cp->trace = static_cast<std::uint8_t *>(std::malloc(kTraceBytes));
  cp->traced = 0;
}

void gx_cp_trace_flush(GxCp *cp)
{
  if (cp == nullptr || cp->trace == nullptr || cp->trace_path == nullptr)
    return;
  std::FILE *f = std::fopen(cp->trace_path, "wb");
  if (f != nullptr)
  {
    std::fwrite(cp->trace, 1, cp->traced, f);
    std::fclose(f);
    std::fprintf(stderr, "gx trace -> %s (%u bytes)\n", cp->trace_path,
                 cp->traced);
  }
}

void gx_cp_set_sink(GxCp *cp, const GxSink *sink)
{
  if (cp == nullptr)
    return;
  if (sink != nullptr)
    cp->sink = *sink;
  else
    cp->sink = GxSink{};
}

const GxRegs *gx_cp_regs(const GxCp *cp) { return cp != nullptr ? &cp->regs : nullptr; }

u32 gx_cp_pending(const GxCp *cp)
{
  return cp != nullptr ? static_cast<u32>(cp->processor.GetBufferedByteCount()) : 0u;
}

u64 gx_cp_commands(const GxCp *cp)
{
  return cp != nullptr ? cp->processor.GetCommandCount() : 0u;
}

u64 gx_cp_unknown(const GxCp *cp)
{
  return cp != nullptr ? cp->processor.GetUnknownOpcodeCount() : 0u;
}

u64 gx_cp_vertices(const GxCp *cp) { return cp != nullptr ? cp->vertices : 0u; }
u64 gx_cp_draws(const GxCp *cp) { return cp != nullptr ? cp->draws : 0u; }
u64 gx_cp_bytes_in(const GxCp *cp) { return cp != nullptr ? cp->bytes_in : 0u; }

int gx_cp_take_finish(GxCp *cp)
{
  if (cp == nullptr || !cp->pe_finish)
    return 0;
  cp->pe_finish = false;
  return 1;
}

int gx_cp_take_token_irq(GxCp *cp)
{
  if (cp == nullptr || !cp->pe_token_irq)
    return 0;
  cp->pe_token_irq = false;
  return 1;
}

int gx_cp_take_token(GxCp *cp, u32 *token)
{
  if (cp == nullptr || !cp->pe_token_set)
    return 0;
  cp->pe_token_set = false;
  if (token != nullptr)
    *token = cp->pe_token;
  return 1;
}
}

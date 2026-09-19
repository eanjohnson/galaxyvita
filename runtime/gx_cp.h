#ifndef GALAXYVITA_GX_CP_H
#define GALAXYVITA_GX_CP_H

#include "common/types.h"
#include "gx_render.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  /* XF is sparse to 0x105F; a flat array that size costs 16 KB and removes
   * every range check from the read path. */
  GX_XF_WORDS = 0x1060u
};

/* The register files the stream has written, in the shape a renderer wants to
 * read them. This is the whole of what the software rasteriser needs to know
 * about the command processor, and it is also what Dolphin's shader generators
 * are driven from -- they take a CP/XF/BP view and nothing else. */
/* Resolves a physical guest address to host bytes, or NULL if `len` bytes are
 * not contiguously available. Textures live in guest RAM and are referenced by
 * address out of BP, so a renderer needs this as much as it needs the
 * registers. */
typedef const u8 *(*GxMemFn)(void *user, u32 phys, u32 len);

typedef struct GxRegs {
  GxMemFn mem_fn;
  void *mem_user;
  u32 bp[256];
  u32 xf[GX_XF_WORDS];
  u32 vcd_low;
  u32 vcd_high;
  u32 vat[8][3];
} GxRegs;

/* Command processor. Wraps ModernGekko's GxCommandProcessor and GxStateBackend:
 * the FIFO parse, display-list following, indexed-attribute fetch and vertex
 * decode are all theirs, and this is the C face the runtime talks to. */
typedef struct GxCp GxCp;

/* mem1/mem2 stay owned by the caller -- on the Vita they are memblocks, not
 * heap -- and must outlive the returned object. */
GxCp *gx_cp_create(void *mem1, u32 mem1_size, void *mem2, u32 mem2_size);
void gx_cp_destroy(GxCp *cp);
void gx_cp_reset(GxCp *cp);

/* Returns nonzero when the write left a PE event for the Flipper to collect.
 * Nearly every write leaves none -- these come from two BP registers and the
 * game writes millions of other bytes between them -- and the caller used to
 * ask three times, across the C++ boundary, after every four-byte store. */
int gx_cp_write(GxCp *cp, u64 value, u8 size);
/* The same stream, as a byte run. This is what the render thread feeds from
 * the ring; the per-write form is the synchronous path's. */
void gx_cp_write_stream(GxCp *cp, const u8 *bytes, u32 count);
/* Whether the parse has produced a PE event the Flipper has not retired.
 * Safe to call from the producer thread while the consumer is parsing. */
int gx_cp_events_pending(const GxCp *cp);
void gx_cp_set_sink(GxCp *cp, const GxSink *sink);

/* Where the GX stack publishes which part of itself is running, for a sampler
 * on another thread; see disc_set_phase_slot, which this follows.
 *
 * One `gx` phase conflated command parse, vertex load and GXM submission, and
 * the nogfx A/B showed the first and last differ by 2.7x -- so a single number
 * ranked none of the work that could be done to any of them. gx_cp.cpp and
 * ModernGekko have no Chassis and must keep having none, so the chassis hands
 * over the byte and the four values, and nothing else crosses. */
void gx_cp_set_phase_slot(volatile u8 *slot, u8 parse, u8 vtxload, u8 submit,
                          u8 copy, u8 dlist);
const GxRegs *gx_cp_regs(const GxCp *cp);

u32 gx_cp_pending(const GxCp *cp);
u64 gx_cp_commands(const GxCp *cp);
u64 gx_cp_unknown(const GxCp *cp);
u64 gx_cp_vertices(const GxCp *cp);
u64 gx_cp_draws(const GxCp *cp);
u64 gx_cp_bytes_in(const GxCp *cp);

/* Pixel Engine events the Flipper turns into interrupts. BP 0x45 is
 * SETDRAWDONE (GXDrawDone); 0x47 and 0x48 record a draw-sync token, and 0x48
 * also asks for an interrupt. Each take() clears what it reports. */
int gx_cp_take_finish(GxCp *cp);
int gx_cp_take_token_irq(GxCp *cp);
int gx_cp_take_token(GxCp *cp, u32 *token);

/* Diagnostic: record the raw bytes fed to the processor so a hang can be
 * replayed off-line instead of waiting out a 20-minute boot. */
u64 gx_cp_display_lists(const GxCp *cp);
u64 gx_cp_display_list_bytes(const GxCp *cp);
u64 gx_cp_display_lists_unresolved(const GxCp *cp);
u64 gx_cp_undecoded(const GxCp *cp);

/* The per-VAT vertex size is recomputed from the VCD and VAT words on every
 * draw command, including twice per draw on the stream path -- once to ask
 * how long the command is and once to run it. Memoise it, with the knob so
 * one build measures both sides. */
void gx_cp_set_vertex_size_cache(GxCp *cp, int on);
/* EFB copies handed to the sink since the run started. Divide the profiler's
 * `gxcopy` sample count by this for milliseconds spent inside each one. */
u32 gx_cp_copies(void);
u64 gx_cp_vsize_hits(const GxCp *cp);
u64 gx_cp_vsize_misses(const GxCp *cp);

/* Command-stream capture, kept deliberately rather than deleted after the
 * first bug it found. Off unless GV_GX_TRACE names a file; records the first
 * 1 MiB of bytes written to the write-gather FIFO, in wire order, and the
 * chassis flushes it on SIGINT/SIGTERM so a hung run still yields its stream.
 * Decoding that capture is what located the opcode the CP was stuck on. */
void gx_cp_trace(GxCp *cp, const char *path);
void gx_cp_trace_flush(GxCp *cp);

#ifdef __cplusplus
}
#endif

#endif

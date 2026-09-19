#ifndef GALAXYVITA_GX_QUEUE_H
#define GALAXYVITA_GX_QUEUE_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The write-gather pipe, decoupled from the thread that fills it.
 *
 * Every byte the guest writes to 0xCC008000 is parsed, vertex-loaded and
 * submitted to GXM on the emulated CPU's own thread, inside the store that
 * produced it. At retraces 1400-2000 that is 96.3 s of a 266 s window --
 * gx 29.7, gxvtx 36.5, gxsub 23.3, gxcopy 4.8, gxdl 2.0 -- none of which can
 * overlap the 125 s of recompiled guest code next to it, because it is the
 * same thread.
 *
 * This is a single-producer single-consumer byte ring between them. The
 * producer is the emulated CPU on CPU0 and does nothing but copy; the
 * consumer is the render thread on CPU1 and runs the entire GX stack.
 *
 * Why this is safe is not "the queue is ordered". The command processor
 * resolves display lists and indexed vertex arrays straight out of guest RAM
 * at parse time, so a deferred parse reads whatever the guest has put there
 * since. That hazard is not introduced here: it is the hardware's. The real
 * GP reads display lists by DMA, asynchronously, and a game that overwrites
 * one without waiting is already broken on a console. What the game waits on
 * is GXDrawDone and draw-sync tokens -- and those are raised *by the
 * consumer*, when it parses the command, not by the producer when it enqueues
 * it. So the guest's own barrier is the queue's barrier, and it means the
 * same thing here as it does on hardware. SMG uses it: 2,275 finishes and
 * 2,764 tokens in a 720 s run.
 *
 * The other half of it is free, and worth naming because it is not obvious.
 * A display list is recorded by the guest into MEM1 -- plain stores on the
 * producer thread, going nowhere near this ring -- and only then is the CALL
 * command written to the FIFO. The consumer reads those list bytes out of
 * MEM1 when it parses the CALL. What makes that safe is the release/acquire
 * pair below: the consumer cannot observe the CALL's bytes without also
 * observing every store the producer made before publishing them, the list
 * among them. The ring orders far more than the ring.
 *
 * Publication is amortised. A release store per guest write is a `dmb ish`
 * on 96 M of them, which is seconds of the window on its own; the head is
 * published every GXQ_PUBLISH_EVERY writes and, unconditionally, whenever the
 * guest reads a Flipper register -- which is where a spin-wait looks, so the
 * consumer cannot be left holding unpublished bytes the guest is waiting on. */

#define GXQ_PUBLISH_EVERY 32u
#define GXQ_STALL_LIMIT 200000000u

typedef struct GxQueue {
  u8 *buf;
  u32 mask;          /* size - 1; size is a power of two */
  /* Shared, and the only two fields that cross threads. */
  volatile u32 head; /* bytes the producer has published */
  volatile u32 tail; /* bytes the consumer has retired */
  /* Producer-private. */
  u32 head_local;
  u32 tail_cache;
  u32 since_pub;
  u64 stalls;        /* pushes that had to wait for the consumer */
  u64 stall_spins;
  u64 stall_spins_at_entry;
  /* Consumer-private. */
  u32 tail_local;
  /* Set by the consumer when the parse produced a PE event, cleared by the
   * producer once it has drained them. */
  volatile u32 events;
  volatile u32 quit;
  volatile u32 running;
  /* Set when the producer has waited far longer than a live consumer could
   * ever take. A consumer that dies leaves the producer spinning on a full
   * ring forever, and an unattended console that hangs costs a power cycle
   * and reports nothing -- so the wait is bounded and the run goes visibly
   * wrong instead of invisibly quiet. */
  volatile u32 wedged;
} GxQueue;

/* size must be a power of two. Returns 0 on allocation failure. */
int gxq_init(GxQueue *q, u8 *storage, u32 size);
void gxq_push(GxQueue *q, u64 value, u8 size);
void gxq_publish(GxQueue *q);
/* Consumer side: how many bytes are readable, and where they start. Returns a
 * contiguous run, which may be shorter than the total when it wraps. */
u32 gxq_peek(GxQueue *q, const u8 **out);
void gxq_consume(GxQueue *q, u32 bytes);
u32 gxq_depth(const GxQueue *q);

/* One iteration of the render thread: take what the ring has, parse it, and
 * retire it. Returns the bytes consumed, 0 when the ring is empty. */
struct GxCp;
u32 gx_render_pump(GxQueue *q, struct GxCp *gx);

#ifdef __cplusplus
}
#endif

#endif

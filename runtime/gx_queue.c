#include "gx_queue.h"

#include <string.h>

int gxq_init(GxQueue *q, u8 *storage, u32 size) {
  if (q == NULL || storage == NULL || size == 0u || (size & (size - 1u)) != 0u)
    return 0;
  memset(q, 0, sizeof(*q));
  q->buf = storage;
  q->mask = size - 1u;
  return 1;
}

void gxq_publish(GxQueue *q) {
  q->since_pub = 0u;
  __atomic_store_n(&q->head, q->head_local, __ATOMIC_RELEASE);
}

/* Free space, from the producer's cached view of the consumer. The cache is
 * what keeps an acquire load off the common path: it is only refreshed when
 * the cached answer says there is not enough room, and it can only ever
 * understate the space available. */
static u32 producer_free(GxQueue *q) {
  return (q->mask + 1u) - (q->head_local - q->tail_cache);
}

void gxq_push(GxQueue *q, u64 value, u8 size) {
  u8 i;
  if (q->wedged)
    return;
  if (producer_free(q) < (u32)size + 1u) {
    q->stall_spins_at_entry = q->stall_spins;
    /* Publish before waiting: the consumer may be idle precisely because the
     * bytes that would unblock it are still sitting unpublished. */
    gxq_publish(q);
    q->stalls++;
    for (;;) {
      q->tail_cache = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
      if (producer_free(q) >= (u32)size + 1u)
        break;
      /* A live consumer drains a full ring in milliseconds. This many spins
       * is seconds of doing nothing else, which only happens when there is
       * no consumer left to wait for. */
      if (++q->stall_spins - q->stall_spins_at_entry > GXQ_STALL_LIMIT) {
        q->wedged = 1u;
        return;
      }
    }
  }
  for (i = 0; i < size; ++i)
    q->buf[(q->head_local + i) & q->mask] =
        (u8)(value >> ((size - i - 1u) * 8u));
  q->head_local += size;
  if (++q->since_pub >= GXQ_PUBLISH_EVERY)
    gxq_publish(q);
}

u32 gxq_peek(GxQueue *q, const u8 **out) {
  const u32 head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
  const u32 avail = head - q->tail_local;
  const u32 off = q->tail_local & q->mask;
  const u32 contiguous = (q->mask + 1u) - off;
  *out = q->buf + off;
  return avail < contiguous ? avail : contiguous;
}

void gxq_consume(GxQueue *q, u32 bytes) {
  q->tail_local += bytes;
  __atomic_store_n(&q->tail, q->tail_local, __ATOMIC_RELEASE);
}

u32 gxq_depth(const GxQueue *q) {
  const u32 head = __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
  const u32 tail = __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
  return head - tail;
}

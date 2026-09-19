/* The byte ring between the emulated CPU and the render thread.
 *
 * A memory-ordering mistake here does not look like a memory-ordering mistake
 * on the console: it looks like one wrong vertex in one frame, hours in, on a
 * thread nobody is watching. So the ring is tested on its own, against a
 * stream whose every byte is checkable, with the producer and consumer on
 * real threads and the run long enough to wrap the buffer thousands of times.
 *
 * The stream is a 64-bit LCG written in the same 1/2/4/8-byte big-endian
 * pieces the write-gather pipe uses, so the test also covers the partial
 * writes and the wrap that splits one value across the end of the buffer. */
#include "gx_queue.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING (64u * 1024u)
#define WRITES 4000000u

static GxQueue q;
static u8 storage[RING];
static volatile int failed;

static u64 lcg(u64 *s) {
  *s = *s * 6364136223846793005ull + 1442695040888963407ull;
  return *s;
}

static u8 size_for(u64 v) {
  switch (v & 3u) {
  case 0: return 1;
  case 1: return 2;
  case 2: return 4;
  default: return 8;
  }
}

static void *producer(void *arg) {
  u64 s = 12345;
  u32 i;
  (void)arg;
  for (i = 0; i < WRITES; ++i) {
    const u64 v = lcg(&s);
    gxq_push(&q, v, size_for(v));
  }
  gxq_publish(&q);
  return NULL;
}

int main(void) {
  pthread_t t;
  u64 s = 12345;
  u32 i, checked = 0;
  if (!gxq_init(&q, storage, RING)) {
    printf("gxq_init failed\n");
    return 1;
  }
  if (pthread_create(&t, NULL, producer, NULL) != 0) {
    printf("pthread_create failed\n");
    return 1;
  }
  for (i = 0; i < WRITES; ++i) {
    const u64 v = lcg(&s);
    const u8 size = size_for(v);
    u8 want[8], got[8];
    u8 n = 0;
    for (u8 k = 0; k < size; ++k)
      want[k] = (u8)(v >> ((size - k - 1u) * 8u));
    /* Byte at a time, because a value can straddle the wrap. */
    while (n < size) {
      const u8 *p;
      u32 avail = gxq_peek(&q, &p);
      if (avail == 0u)
        continue;
      if (avail > (u32)(size - n))
        avail = (u32)(size - n);
      memcpy(got + n, p, avail);
      gxq_consume(&q, avail);
      n = (u8)(n + avail);
    }
    if (memcmp(want, got, size) != 0) {
      printf("mismatch at write %u: size=%u\n", i, size);
      failed = 1;
      break;
    }
    checked++;
  }
  pthread_join(t, NULL);
  if (failed)
    return 1;
  if (checked != WRITES) {
    printf("checked %u of %u\n", checked, WRITES);
    return 1;
  }
  if (gxq_depth(&q) != 0u) {
    printf("ring not drained: %u bytes left\n", gxq_depth(&q));
    return 1;
  }
  printf("gx_queue: %u writes, %llu producer stalls, %llu spins, ring %u\n",
         checked, (unsigned long long)q.stalls,
         (unsigned long long)q.stall_spins, RING);
  return 0;
}

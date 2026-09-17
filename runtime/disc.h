#ifndef GALAXYVITA_DISC_H
#define GALAXYVITA_DISC_H

#include "common/types.h"

#include <stdio.h>

/* Disc backend over an extracted game tree (wit EXTRACT layout):
 *
 *   <root>/sys/boot.bin      partition byte 0x000000 .. 0x000440
 *   <root>/sys/bi2.bin       partition byte 0x000440 .. 0x002440
 *   <root>/sys/apploader.img partition byte 0x002440 .. 0x002440+size
 *   <root>/sys/main.dol      partition byte boot.bin[0x420] << 2
 *   <root>/sys/fst.bin       partition byte boot.bin[0x424] << 2
 *   <root>/files/<path>      partition byte (FST entry offset) << 2
 *
 * Offsets handed to disc_read() are partition-relative *bytes*, i.e. what
 * DVDLowRead's command word 2 carries once it has been shifted left by two.
 * The tree holds the user's own disc; no bytes of it live in this repo. */

typedef struct DiscEntry {
  u64 offset;
  u32 size;
  char *path; /* host path, owned */
  /* First-touch order. Which files the game pulls, in the order it pulls them,
   * is the cheapest read on how far into the boot a run actually gets. */
  u32 first_seen;
} DiscEntry;

typedef struct Disc {
  char root[512];
  DiscEntry *entries; /* offset-sorted: sys blobs + every FST file */
  u32 count;
  u8 *boot;  /* 0x440 bytes */
  u8 *bi2;   /* 0x2000 bytes */
  u8 *fst;   /* fst_size bytes, kept for the apploader HLE */
  u32 fst_size;
  u32 fst_max_size;
  u64 fst_offset;
  u64 dol_offset;
  u32 dol_size;
  /* Raw (pre-partition) system area: disc header, partition table, region.
   * DVDLowUnencryptedRead is only allowed to touch this. */
  u8 *sysarea;
  u32 sysarea_size;
  u64 partition_offset;
  u8 *tmd; /* <root>/tmd.bin, handed back by DVDLowOpenPartition */
  u32 tmd_size;
  /* One-entry host file cache: DVD reads walk a file front to back. */
  FILE *cache_fp;
  u32 cache_idx;
  /* Counters (evidence for PROGRESS.md, not decoration). */
  u32 reads;
  u32 misses;
  /* Which entries a read wanted and could not open. A read spans every FST
   * entry overlapping its byte range, so a staged subset missing one file
   * fails reads aimed at its neighbours too -- and "misses=49" on its own
   * names none of them. The name is the whole diagnosis. */
  u32 miss_idx[32];
  u32 miss_n;
  /* GV_DISC_TRACE: print every read and the entries it spans. */
  int trace;
  /* Bumped on every entry's first read; see DiscEntry::first_seen. */
  u32 seq;
  u64 bytes;
  int opened;
} Disc;

int disc_open(Disc *d, const char *root);
/* Copy `len` bytes at partition-relative `offset` into `dst`. Regions with no
 * backing file read as zero. Returns 1 if every byte was backed. */
int disc_read(Disc *d, u64 offset, u32 len, u8 *dst);
/* Same, but for raw whole-disc byte offsets (DVDLowUnencryptedRead). Only the
 * 0x00000..0x50000 system area is backed; everything else reads as zero and
 * returns 0 so the caller can answer with a drive error, like a real disc. */
int disc_read_raw(Disc *d, u64 offset, u32 len, u8 *dst);
void disc_close(Disc *d);

#endif

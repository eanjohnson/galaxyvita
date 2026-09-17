#include "disc.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static u32 be32(const u8 *p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void put_be32(u8 *p, u32 v) {
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}

static u8 *read_file(const char *path, u32 *size_out, u32 want) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 0) {
    fclose(f);
    return NULL;
  }
  u32 size = want && (u32)n > want ? want : (u32)n;
  u8 *buf = (u8 *)malloc(size ? size : 1u);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  if (size && fread(buf, 1, size, f) != size) {
    free(buf);
    fclose(f);
    return NULL;
  }
  fclose(f);
  if (size_out)
    *size_out = size;
  return buf;
}

static long host_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

static char *dup_str(const char *s) {
  size_t n = strlen(s) + 1u;
  char *p = (char *)malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

static int push(Disc *d, u32 *cap, u64 offset, u32 size, const char *path) {
  if (d->count == *cap) {
    u32 grow = *cap ? *cap * 2u : 64u;
    DiscEntry *e = (DiscEntry *)realloc(d->entries, grow * sizeof(*e));
    if (!e)
      return 0;
    d->entries = e;
    *cap = grow;
  }
  DiscEntry *e = &d->entries[d->count];
  e->offset = offset;
  e->size = size;
  /* realloc does not zero, and first_seen is only ever assigned when it reads
   * back as zero. An entry that landed on dirty memory therefore stays
   * "already seen" for the whole run and never enters the census -- so the
   * file is never staged, and the console alone hits a disc read the host
   * always satisfied. That is a boot wedge two steps removed from its cause. */
  e->first_seen = 0;
  e->path = dup_str(path);
  if (!e->path)
    return 0;
  d->count++;
  return 1;
}

static int cmp_entry(const void *a, const void *b) {
  const DiscEntry *x = (const DiscEntry *)a, *y = (const DiscEntry *)b;
  if (x->offset < y->offset)
    return -1;
  if (x->offset > y->offset)
    return 1;
  return 0;
}

/* Walk the FST and register every file with its host path. Directory entries
 * carry their end index in word 2, which is all the nesting state we need. */
static int walk_fst(Disc *d, u32 *cap) {
  const u8 *fst = d->fst;
  if (!fst || d->fst_size < 12u)
    return 0;
  u32 n = be32(fst + 8);
  u64 strtab = (u64)n * 12u;
  if (n == 0 || strtab >= d->fst_size)
    return 0;

  char dirs[16][256];
  u32 ends[16];
  u32 depth = 0;
  char path[1024];
  for (u32 i = 1; i < n; i++) {
    const u8 *e = fst + (size_t)i * 12u;
    while (depth && i >= ends[depth - 1])
      depth--;
    u32 name_off = be32(e) & 0x00FFFFFFu;
    u32 w1 = be32(e + 4);
    u32 w2 = be32(e + 8);
    if (strtab + name_off >= d->fst_size)
      return 0;
    const char *name = (const char *)fst + strtab + name_off;
    if (e[0] == 1) {
      if (depth < 16) {
        snprintf(dirs[depth], sizeof(dirs[0]), "%s", name);
        ends[depth] = w2;
        depth++;
      }
      continue;
    }
    int len = snprintf(path, sizeof(path), "%s/files", d->root);
    for (u32 k = 0; k < depth && len > 0 && len < (int)sizeof(path); k++)
      len += snprintf(path + len, sizeof(path) - (size_t)len, "/%s", dirs[k]);
    if (len > 0 && len < (int)sizeof(path))
      len += snprintf(path + len, sizeof(path) - (size_t)len, "/%s", name);
    if (len <= 0 || len >= (int)sizeof(path))
      return 0;
    if (!push(d, cap, (u64)w1 << 2, w2, path))
      return 0;
  }
  return 1;
}

int disc_open(Disc *d, const char *root) {
  memset(d, 0, sizeof(*d));
  snprintf(d->root, sizeof(d->root), "%s", root);

  char path[1024];
  u32 cap = 0;

  snprintf(path, sizeof(path), "%s/sys/boot.bin", d->root);
  u32 boot_size = 0;
  d->boot = read_file(path, &boot_size, 0x440u);
  if (!d->boot || boot_size < 0x440u) {
    disc_close(d);
    return 0;
  }
  if (!push(d, &cap, 0, 0x440u, path)) {
    disc_close(d);
    return 0;
  }

  snprintf(path, sizeof(path), "%s/sys/bi2.bin", d->root);
  u32 bi2_size = 0;
  d->bi2 = read_file(path, &bi2_size, 0x2000u);
  if (d->bi2 && !push(d, &cap, 0x440u, bi2_size, path)) {
    disc_close(d);
    return 0;
  }

  snprintf(path, sizeof(path), "%s/sys/apploader.img", d->root);
  long apl = host_size(path);
  if (apl > 0 && !push(d, &cap, 0x2440u, (u32)apl, path)) {
    disc_close(d);
    return 0;
  }

  d->dol_offset = (u64)be32(d->boot + 0x420) << 2;
  d->fst_offset = (u64)be32(d->boot + 0x424) << 2;
  d->fst_size = be32(d->boot + 0x428) << 2;
  d->fst_max_size = be32(d->boot + 0x42C) << 2;

  snprintf(path, sizeof(path), "%s/sys/main.dol", d->root);
  long dol = host_size(path);
  if (dol <= 0) {
    disc_close(d);
    return 0;
  }
  d->dol_size = (u32)dol;
  if (!push(d, &cap, d->dol_offset, d->dol_size, path)) {
    disc_close(d);
    return 0;
  }

  snprintf(path, sizeof(path), "%s/sys/fst.bin", d->root);
  u32 got = 0;
  d->fst = read_file(path, &got, 0);
  if (!d->fst || got == 0) {
    disc_close(d);
    return 0;
  }
  if (d->fst_size > got)
    d->fst_size = got;
  if (!push(d, &cap, d->fst_offset, d->fst_size, path)) {
    disc_close(d);
    return 0;
  }

  if (!walk_fst(d, &cap)) {
    disc_close(d);
    return 0;
  }

  /* Rebuild the raw system area the drive exposes before any partition is
   * opened: disc header at 0, partition table at 0x40000, region at 0x4E000,
   * and the magic word the SDK checks at 0x4FFFC. */
  d->partition_offset = 0xF800000ull;
  d->sysarea_size = 0x50000u;
  d->sysarea = (u8 *)calloc(1, d->sysarea_size);
  if (!d->sysarea) {
    disc_close(d);
    return 0;
  }
  {
    u32 got2 = 0;
    snprintf(path, sizeof(path), "%s/disc/header.bin", d->root);
    u8 *hdr = read_file(path, &got2, 0x100u);
    if (hdr) {
      memcpy(d->sysarea, hdr, got2 < 0x100u ? got2 : 0x100u);
      free(hdr);
    } else {
      memcpy(d->sysarea, d->boot, 0x100u);
    }
    got2 = 0;
    snprintf(path, sizeof(path), "%s/disc/region.bin", d->root);
    u8 *reg = read_file(path, &got2, 0x20u);
    if (reg) {
      memcpy(d->sysarea + 0x4E000u, reg, got2 < 0x20u ? got2 : 0x20u);
      free(reg);
    }
    u8 *pt = d->sysarea + 0x40000u;
    put_be32(pt, 1u);                                  /* one partition group */
    put_be32(pt + 4, 0x40020u >> 2);                   /* table offset >> 2 */
    put_be32(pt + 0x20, (u32)(d->partition_offset >> 2));
    put_be32(pt + 0x24, 0u);                           /* type: game data */
    put_be32(d->sysarea + 0x4FFFCu, 0xC3F81A8Eu);      /* Wii partition magic */
  }

  snprintf(path, sizeof(path), "%s/tmd.bin", d->root);
  d->tmd = read_file(path, &d->tmd_size, 0);

  qsort(d->entries, d->count, sizeof(d->entries[0]), cmp_entry);
  d->cache_idx = 0xFFFFFFFFu;
  d->opened = 1;
  d->trace = getenv("GV_DISC_TRACE") != NULL;
  return 1;
}

/* First entry whose end is past `offset`. */
static u32 lower_bound(const Disc *d, u64 offset) {
  u32 lo = 0, hi = d->count;
  while (lo < hi) {
    u32 mid = lo + (hi - lo) / 2u;
    const DiscEntry *e = &d->entries[mid];
    if (e->offset + e->size <= offset)
      lo = mid + 1u;
    else
      hi = mid;
  }
  return lo;
}

static FILE *entry_fp(Disc *d, u32 idx) {
  if (d->cache_idx == idx && d->cache_fp)
    return d->cache_fp;
  if (d->cache_fp)
    fclose(d->cache_fp);
  d->cache_fp = fopen(d->entries[idx].path, "rb");
  d->cache_idx = d->cache_fp ? idx : 0xFFFFFFFFu;
  return d->cache_fp;
}

/* Wall-clock spent inside guest-visible file reads, and how many there were.
 * On the host these come out of the page cache and cost nothing, which is why
 * a 2.11x CPU win showed there and not on the console: the question "is the
 * device CPU-bound in this phase at all?" cannot be answered without this. */
u64 g_disc_io_us;
u32 g_disc_io_n;

static int disc_read_inner(Disc *d, u64 offset, u32 len, u8 *dst) {
  if (!d->opened || !dst)
    return 0;
  memset(dst, 0, len);
  d->reads++;
  d->bytes += len;
  if (d->trace) {
    fprintf(stderr, "discread #%u off=0x%llx len=%u ->", d->reads,
            (unsigned long long)offset, len);
    for (u32 i = lower_bound(d, offset); i < d->count; i++) {
      DiscEntry *e = &d->entries[i];
      if (e->offset >= offset + len)
        break;
      if (e->offset + e->size <= offset)
        continue;
      const char *b = strrchr(e->path, '/');
      fprintf(stderr, " %s[off=0x%llx sz=%u seen=%u]", b ? b + 1 : e->path,
              (unsigned long long)e->offset, e->size, e->first_seen);
    }
    fprintf(stderr, "\n");
  }

  u64 end = offset + len;
  int complete = 1;
  u64 covered = offset;
  for (u32 i = lower_bound(d, offset); i < d->count; i++) {
    DiscEntry *e = &d->entries[i];
    if (e->offset >= end)
      break;
    if (e->offset > covered)
      complete = 0; /* gap: padding between files, reads as zero */
    u64 from = offset > e->offset ? offset : e->offset;
    u64 to = end < e->offset + e->size ? end : e->offset + e->size;
    if (to <= from)
      continue;
    FILE *f = entry_fp(d, i);
    if (!f) {
      complete = 0;
      u32 k = 0;
      while (k < d->miss_n && d->miss_idx[k] != i)
        k++;
      if (k == d->miss_n && d->miss_n < 32u)
        d->miss_idx[d->miss_n++] = i;
      continue;
    }
    if (fseek(f, (long)(from - e->offset), SEEK_SET) != 0 ||
        fread(dst + (from - offset), 1, (size_t)(to - from), f) !=
            (size_t)(to - from)) {
      complete = 0;
      continue;
    }
    if (!e->first_seen) {
      e->first_seen = ++d->seq;
    }
    covered = to;
  }
  if (covered < end)
    complete = 0;
  if (!complete)
    d->misses++;
  return complete;
}

static u64 disc_now_us(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;
  return (u64)ts.tv_sec * 1000000u + (u64)(ts.tv_nsec / 1000);
}

int disc_read(Disc *d, u64 offset, u32 len, u8 *dst) {
  const u64 t0 = disc_now_us();
  const int rc = disc_read_inner(d, offset, len, dst);
  g_disc_io_us += disc_now_us() - t0;
  g_disc_io_n++;
  return rc;
}

int disc_read_raw(Disc *d, u64 offset, u32 len, u8 *dst) {
  if (!d->opened || !dst)
    return 0;
  memset(dst, 0, len);
  if (!d->sysarea || offset >= d->sysarea_size)
    return 0;
  u64 end = offset + len;
  if (end > d->sysarea_size)
    end = d->sysarea_size;
  memcpy(dst, d->sysarea + offset, (size_t)(end - offset));
  d->reads++;
  d->bytes += len;
  return end == offset + len;
}

void disc_close(Disc *d) {
  if (d->cache_fp)
    fclose(d->cache_fp);
  d->cache_fp = NULL;
  for (u32 i = 0; i < d->count; i++)
    free(d->entries[i].path);
  free(d->entries);
  free(d->boot);
  free(d->bi2);
  free(d->fst);
  free(d->sysarea);
  free(d->tmd);
  d->sysarea = NULL;
  d->tmd = NULL;
  d->entries = NULL;
  d->boot = d->bi2 = d->fst = NULL;
  d->count = 0;
  d->opened = 0;
}

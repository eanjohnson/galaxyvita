#include "nand.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Guest paths are absolute and must stay inside the NAND root: IOS has no
 * notion of "..", so anything containing one is a bug or an attack. */
static int host_path(const Nand *n, const char *guest, char *out, u32 cap) {
  if (!guest || guest[0] != '/')
    return 0;
  if (strstr(guest, ".."))
    return 0;
  int len = snprintf(out, cap, "%s%s", n->root, guest);
  return len > 0 && (u32)len < cap;
}

static int mkdir_p(const char *path) {
  char buf[1024];
  int len = snprintf(buf, sizeof(buf), "%s", path);
  if (len <= 0 || (u32)len >= sizeof(buf))
    return 0;
  for (char *p = buf + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = 0;
    mkdir(buf, 0755);
    *p = '/';
  }
  mkdir(buf, 0755);
  return 1;
}

int nand_open(Nand *n, const char *root) {
  memset(n, 0, sizeof(*n));
  snprintf(n->root, sizeof(n->root), "%s", root);
  size_t len = strlen(n->root);
  while (len && n->root[len - 1] == '/')
    n->root[--len] = 0;
  struct stat st;
  if (stat(n->root, &st) != 0 || !S_ISDIR(st.st_mode))
    return 0;
  n->opened = 1;
  return 1;
}

void nand_close(Nand *n) {
  for (int i = 0; i < NAND_HANDLES; i++) {
    if (n->h[i].fp)
      fclose(n->h[i].fp);
    n->h[i].fp = NULL;
    n->h[i].used = 0;
  }
  n->opened = 0;
}

static NandHandle *get(Nand *n, s32 h) {
  if (h < 0 || h >= NAND_HANDLES || !n->h[h].used)
    return NULL;
  return &n->h[h];
}

s32 nand_file_open(Nand *n, const char *guest_path, int mode) {
  char path[1024];
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  int slot = -1;
  for (int i = 0; i < NAND_HANDLES; i++) {
    if (!n->h[i].used) {
      slot = i;
      break;
    }
  }
  if (slot < 0)
    return NAND_ENFILE;

  struct stat st;
  int exists = stat(path, &st) == 0 && S_ISREG(st.st_mode);
  if (!exists) {
    n->misses++;
    return NAND_ENOENT;
  }
  /* IOS never truncates on open; r+b for anything that can write. */
  FILE *f = fopen(path, (mode & NAND_MODE_WRITE) ? "r+b" : "rb");
  if (!f)
    return NAND_EACCES;

  NandHandle *h = &n->h[slot];
  h->fp = f;
  h->size = (u32)st.st_size;
  h->used = 1;
  h->mode = mode;
  snprintf(h->path, sizeof(h->path), "%s", guest_path);
  n->opens++;
  return slot;
}

s32 nand_file_close(Nand *n, s32 idx) {
  NandHandle *h = get(n, idx);
  if (!h)
    return NAND_EINVAL;
  if (h->fp)
    fclose(h->fp);
  memset(h, 0, sizeof(*h));
  return NAND_OK;
}

s32 nand_file_read(Nand *n, s32 idx, void *dst, u32 len) {
  NandHandle *h = get(n, idx);
  if (!h || !h->fp)
    return NAND_EINVAL;
  if (!(h->mode & NAND_MODE_READ))
    return NAND_EACCES;
  size_t got = len ? fread(dst, 1, len, h->fp) : 0;
  n->reads++;
  return (s32)got;
}

s32 nand_file_write(Nand *n, s32 idx, const void *src, u32 len) {
  NandHandle *h = get(n, idx);
  if (!h || !h->fp)
    return NAND_EINVAL;
  if (!(h->mode & NAND_MODE_WRITE))
    return NAND_EACCES;
  size_t put = len ? fwrite(src, 1, len, h->fp) : 0;
  fflush(h->fp);
  long pos = ftell(h->fp);
  if (pos > 0 && (u32)pos > h->size)
    h->size = (u32)pos;
  n->writes++;
  return (s32)put;
}

s32 nand_file_seek(Nand *n, s32 idx, s32 offset, u32 whence) {
  NandHandle *h = get(n, idx);
  if (!h || !h->fp)
    return NAND_EINVAL;
  int w = whence == 1u ? SEEK_CUR : (whence == 2u ? SEEK_END : SEEK_SET);
  if (fseek(h->fp, offset, w) != 0)
    return NAND_EINVAL;
  long pos = ftell(h->fp);
  if (pos < 0)
    return NAND_EINVAL;
  return (s32)pos;
}

s32 nand_file_stats(Nand *n, s32 idx, u32 *size, u32 *pos) {
  NandHandle *h = get(n, idx);
  if (!h || !h->fp)
    return NAND_EINVAL;
  long at = ftell(h->fp);
  if (size)
    *size = h->size;
  if (pos)
    *pos = at < 0 ? 0u : (u32)at;
  return NAND_OK;
}

s32 nand_create_file(Nand *n, const char *guest_path) {
  char path[1024];
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  struct stat st;
  if (stat(path, &st) == 0)
    return NAND_EEXIST;
  char *slash = strrchr(path, '/');
  if (slash) {
    *slash = 0;
    mkdir_p(path);
    *slash = '/';
  }
  FILE *f = fopen(path, "wb");
  if (!f)
    return NAND_EACCES;
  fclose(f);
  return NAND_OK;
}

s32 nand_create_dir(Nand *n, const char *guest_path) {
  char path[1024];
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  struct stat st;
  if (stat(path, &st) == 0)
    return NAND_EEXIST;
  return mkdir_p(path) ? NAND_OK : NAND_EACCES;
}

s32 nand_delete(Nand *n, const char *guest_path) {
  char path[1024];
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  if (remove(path) != 0)
    return NAND_ENOENT;
  return NAND_OK;
}

s32 nand_rename(Nand *n, const char *from, const char *to) {
  char a[1024], b[1024];
  if (!n->opened || !host_path(n, from, a, sizeof(a)) ||
      !host_path(n, to, b, sizeof(b)))
    return NAND_EINVAL;
  char *slash = strrchr(b, '/');
  if (slash) {
    *slash = 0;
    mkdir_p(b);
    *slash = '/';
  }
  if (rename(a, b) != 0)
    return NAND_ENOENT;
  return NAND_OK;
}

s32 nand_exists(Nand *n, const char *guest_path) {
  char path[1024];
  struct stat st;
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  if (stat(path, &st) != 0)
    return NAND_ENOENT;
  return S_ISDIR(st.st_mode) ? 1 : 0;
}

s32 nand_read_dir(Nand *n, const char *guest_path, char *names, u32 max,
                  u32 *total) {
  char path[1024];
  if (!n->opened || !host_path(n, guest_path, path, sizeof(path)))
    return NAND_EINVAL;
  DIR *dir = opendir(path);
  if (!dir)
    return NAND_ENOENT;
  u32 count = 0, packed = 0, used = 0;
  u32 cap = max * 13u;
  if (names)
    memset(names, 0, cap);
  struct dirent *de;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    count++;
    if (!names || packed >= max)
      continue;
    u32 len = (u32)strlen(de->d_name);
    if (len > 12u)
      len = 12u;
    /* IOS packs the names: name, NUL, next name. Not a 13-byte stride. */
    if (used + len + 1u > cap)
      continue;
    memcpy(names + used, de->d_name, len);
    names[used + len] = 0;
    used += len + 1u;
    packed++;
  }
  closedir(dir);
  if (total)
    *total = count;
  return (s32)packed;
}

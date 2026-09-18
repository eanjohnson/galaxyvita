#include "ios.h"
#include "cpu/cpu.h"
#include "disc.h"
#include "flipper.h"
#include "nand.h"

#include <stdio.h>
#include <string.h>

#define IPC_CMD_OPEN 1u
#define IPC_CMD_CLOSE 2u
#define IPC_CMD_READ 3u
#define IPC_CMD_WRITE 4u
#define IPC_CMD_SEEK 5u
#define IPC_CMD_IOCTL 6u
#define IPC_CMD_IOCTLV 7u
#define IPC_REPLY 8u

#define IPC_SUCCESS 0
#define IPC_ENOENT (-6)
#define IPC_EINVAL (-4)
#define IPC_EMAX (-5)

#define PI_CAUSE_IPC 0x4000u

#define TITLE_HI 0x00010000u
#define TITLE_LO 0x524D4745u /* RMGE */

#define IOCTL_ES_GETDEVICEID 0x07u
#define IOCTL_ES_GETTITLEDIR 0x1Du
#define IOCTL_ES_GETTITLEID 0x20u
#define IOCTL_ES_SETUID 0x21u
#define IOCTL_ES_GETBOOT2VERSION 0x2Eu
#define IOCTL_ES_CHECKKOREAREGION 0x45u
#define IOCTL_ES_GETCONSUMPTION 0x16u
#define IOCTL_ES_DIGETTICKETVIEW 0x1Bu
/* ES::TicketView, the v0 layout IOS hands back. */
#define ES_TICKET_VIEW_SIZE 0xD8u

#define IOCTL_STM_EVENTHOOK 0x1000u
#define IOCTL_STM_HOTRESET 0x2001u
#define IOCTL_STM_SHUTDOWN 0x2003u
#define IOCTL_STM_IDLE 0x2004u
#define IOCTL_STM_RELEASE_EH 0x3002u
#define IOCTL_STM_VIDIMMING 0x5001u
#define IOCTL_STM_LEDMODE 0x6002u

/* DI replies are DIResult, where success is 1 — not the IPC 0 every other
 * device uses. Returning 0 reads as "no result" to the SDK's DVD driver. */
#define DI_SUCCESS 1
#define DI_DRIVE_ERROR 2
#define DI_COVER_CLOSED 4
#define DI_SECURITY_ERROR 32
#define DI_BAD_ARGUMENT 128

/* Drive error codes (Dolphin HW/DVD/DVDInterface.h plus the DI firmware's
 * 0x053100 for an unencrypted read of an address outside the allowed
 * windows). RMGE01 only accepts 0x053100 or 0x052000 there. */
#define DI_ERR_NONE 0x000000u
/* Drive error codes, as reported by DVDLowRequestError. RMGE01's anti-modchip
 * check (0x804C5764) runs two probes and demands an exact code from each:
 *   round 1: DVDLowUnencryptedRead of 0x460A0000 -> 0x052100 (BlockOOB)
 *   round 2: DVDLowReportKey                     -> 0x052000 (InvalidCommand)
 * Anything else, including success, black-screens into OSFatal "Error #001". */
#define DI_ERR_BLOCK_OOB 0x052100u
#define DI_ERR_INVALID_COMMAND 0x052000u

#define DVDLowInquiry 0x12u
#define DVDLowReadDiskID 0x70u
#define DVDLowRead 0x71u
#define DVDLowGetCoverRegister 0x7au
#define DVDLowGetCoverStatus 0x88u
#define DVDLowReset 0x8au
#define DVDLowClosePartition 0x8cu
#define DVDLowUnencryptedRead 0x8du
#define DVDLowClearCoverInterrupt 0x86u
#define DVDLowWaitForCoverClose 0x79u
#define DVDLowNotifyReset 0x7eu
#define DVDLowGetLength 0x83u
#define DVDLowGetImmBuf 0x84u
#define DVDLowMaskCoverInterrupt 0x85u
#define DVDLowUnmaskStatusInterrupts 0x87u
#define DVDLowUnmaskCoverInterrupt 0x89u
#define DVDLowOpenPartition 0x8bu
#define DVDLowReportKey 0xA4u

#define BT_IOCTLV_CTRLMSG 0u
#define BT_IOCTLV_BLKMSG 1u
#define BT_IOCTLV_INTRMSG 2u
#define DVDLowSeek 0xABu
#define DVDLowRequestError 0xE0u
#define DVDLowStopMotor 0xE3u

/* ISFS ioctls (Dolphin IOS/FS/FileSystemProxy.h). */
#define ISFS_IOCTL_FORMAT 1u
#define ISFS_IOCTL_GETSTATS 2u
#define ISFS_IOCTL_CREATEDIR 3u
#define ISFS_IOCTLV_READDIR 4u
#define ISFS_IOCTL_SETATTR 5u
#define ISFS_IOCTL_GETATTR 6u
#define ISFS_IOCTL_DELETE 7u
#define ISFS_IOCTL_RENAME 8u
#define ISFS_IOCTL_CREATEFILE 9u
#define ISFS_IOCTL_SETFILEVERCTRL 10u
#define ISFS_IOCTL_GETFILESTATS 11u
#define ISFS_IOCTLV_GETUSAGE 12u
#define ISFS_IOCTL_SHUTDOWN 13u

/* ISFSParams: uid u32, gid u16, path[64], modes[3], attribute u8. */
#define ISFS_PARAMS_SIZE 74u

static u32 to_ea(u32 a) {
  if (a == 0)
    return 0;
  /* Physical MEM1 / MEM2 (Starlet / PPCMSG). */
  if (a < 0x01800000u)
    return a | 0x80000000u;
  if (a >= 0x10000000u && a < 0x14000000u)
    return (a - 0x10000000u) + 0x90000000u;
  /* Uncached mirrors. */
  if (a >= 0xC0000000u && a < 0xC1800000u)
    return 0x80000000u + (a - 0xC0000000u);
  if (a >= 0xD0000000u && a < 0xD4000000u)
    return 0x90000000u + (a - 0xD0000000u);
  return a;
}

static void write_bytes(CPUState *cpu, u32 ea, const void *src, u32 n) {
  const u8 *p = (const u8 *)src;
  for (u32 i = 0; i < n; i++)
    mem_write8(cpu, ea + i, p[i]);
}

static void write_str(CPUState *cpu, u32 ea, const char *s) {
  u32 i = 0;
  while (s[i]) {
    mem_write8(cpu, ea + i, (u8)s[i]);
    i++;
  }
  mem_write8(cpu, ea + i, 0);
}

static void read_bytes(CPUState *cpu, u32 ea, void *dst, u32 n) {
  u8 *p = (u8 *)dst;
  for (u32 i = 0; i < n; i++)
    p[i] = mem_read8(cpu, ea + i);
}

static void read_str(CPUState *cpu, u32 ea, char *out, u32 cap) {
  u32 i = 0;
  if (!ea || !cap) {
    if (cap)
      out[0] = 0;
    return;
  }
  for (; i + 1 < cap; i++) {
    u8 c = mem_read8(cpu, ea + i);
    out[i] = (char)c;
    if (!c)
      return;
  }
  out[cap - 1] = 0;
}

static int looks_like_path(const char *s) {
  return s && s[0] == '/';
}

/* DVDLowInit / NET copy the path into an IPC-bump buffer, then IOS_Open
 * stores a physical pointer. Try every Wii mirror, then +0x20/+0x40 in
 * case the string sits after the 32-byte request header. */
static int try_read_path(CPUState *cpu, u32 raw, char *out, u32 cap) {
  u32 cands[8];
  u32 n = 0;
  u32 i;
  if (cap)
    out[0] = 0;
  if (!raw || !cap)
    return 0;
  cands[n++] = to_ea(raw);
  cands[n++] = raw;
  if ((raw & 0xF0000000u) == 0x90000000u) {
    cands[n++] = 0xD0000000u | (raw & 0x0FFFFFFFu);
    cands[n++] = 0x10000000u + (raw & 0x0FFFFFFFu);
  }
  if ((raw & 0xF0000000u) == 0xD0000000u)
    cands[n++] = 0x90000000u | (raw & 0x0FFFFFFFu);
  if ((raw & 0xF0000000u) == 0x10000000u)
    cands[n++] = 0x90000000u + (raw - 0x10000000u);
  for (i = 0; i < n; i++) {
    u32 ea = cands[i];
    if (!ea)
      continue;
    read_str(cpu, ea, out, cap);
    if (looks_like_path(out))
      return 1;
    read_str(cpu, ea + 0x20u, out, cap);
    if (looks_like_path(out))
      return 1;
    read_str(cpu, ea + 0x40u, out, cap);
    if (looks_like_path(out))
      return 1;
  }
  if (cap)
    out[0] = 0;
  return 0;
}

void ios_note_open_arg(Ios *ios, CPUState *cpu, u32 path_ea) {
  ios->pending_path_ea = path_ea;
  ios->pending_path[0] = 0;
  if (!path_ea)
    return;
  if (!try_read_path(cpu, path_ea, ios->pending_path, IOS_PATH_MAX))
    read_str(cpu, to_ea(path_ea), ios->pending_path, IOS_PATH_MAX);
  if (ios->log_left && ios->log) {
    ios->log_left--;
    char buf[128];
    snprintf(buf, sizeof(buf), "ios open-arg ea=0x%x lr=0x%x '%s'",
             (unsigned)path_ea, (unsigned)cpu->lr,
             ios->pending_path[0] ? ios->pending_path : "(empty)");
    ios->log(buf, ios->log_user);
  }
}

static int match_dev(const char *path) {
  if (!path || path[0] == 0)
    return IOS_DEV_NONE;
  if (strcmp(path, "/dev/es") == 0)
    return IOS_DEV_ES;
  if (strcmp(path, "/dev/fs") == 0)
    return IOS_DEV_FS;
  if (strcmp(path, "/dev/di") == 0)
    return IOS_DEV_DI;
  if (strcmp(path, "/dev/stm/immediate") == 0)
    return IOS_DEV_STM_IMM;
  if (strcmp(path, "/dev/stm/eventhook") == 0)
    return IOS_DEV_STM_EH;
  if (strcmp(path, "/dev/usb/oh1/57e/305") == 0)
    return IOS_DEV_BT;
  if (strcmp(path, "/dev/usb/oh1") == 0)
    return IOS_DEV_OH1;
  if (strncmp(path, "/dev/net/", 9) == 0)
    return IOS_DEV_NET;
  if (strncmp(path, "/dev/sdio/", 10) == 0)
    return IOS_DEV_SDIO;
  if (strncmp(path, "/dev/usb/", 9) == 0)
    return IOS_DEV_USB;
  if (strcmp(path, "/dev/aes") == 0)
    return IOS_DEV_AES;
  if (strcmp(path, "/dev/sha") == 0)
    return IOS_DEV_SHA;
  if (path[0] == '/')
    return IOS_DEV_FS;
  return IOS_DEV_NONE;
}

static s32 alloc_fd(Ios *ios, int dev) {
  for (u32 i = 0; i < IOS_FD_MAX; i++) {
    if (ios->fd_dev[i] == IOS_DEV_NONE) {
      ios->fd_dev[i] = dev;
      return (s32)i;
    }
  }
  return IPC_EMAX;
}

static int fd_dev(Ios *ios, u32 fd) {
  if (fd >= IOS_FD_MAX)
    return IOS_DEV_NONE;
  return ios->fd_dev[fd];
}

static s32 handle_es(CPUState *cpu, u32 request, u32 out, u32 out_size) {
  (void)out_size;
  switch (request) {
  case IOCTL_ES_GETTITLEID:
    if (out) {
      mem_write32(cpu, out, TITLE_HI);
      mem_write32(cpu, out + 4, TITLE_LO);
    }
    return IPC_SUCCESS;
  case IOCTL_ES_GETTITLEDIR:
    if (out)
      write_str(cpu, out, "/title/00010000/524d4745/data");
    return IPC_SUCCESS;
  case IOCTL_ES_GETDEVICEID:
    if (out)
      mem_write32(cpu, out, 0x0408A001u);
    return IPC_SUCCESS;
  case IOCTL_ES_GETBOOT2VERSION:
    if (out)
      mem_write32(cpu, out, 4);
    return IPC_SUCCESS;
  case IOCTL_ES_SETUID:
  case IOCTL_ES_CHECKKOREAREGION:
    return IPC_SUCCESS;
  default:
    return IPC_SUCCESS;
  }
}

/* Copy `len` disc bytes into the guest's output buffer. Regions with no
 * backing file read as zero; the miss counter is the evidence that a read
 * fell off the extracted tree. */
static s32 di_copy(Ios *ios, CPUState *cpu, u64 offset, u32 len, u32 out,
                   u32 out_size, int raw) {
  if (!ios->disc)
    return DI_DRIVE_ERROR;
  if (!out || out_size < len)
    return DI_SECURITY_ERROR;
  u8 chunk[4096];
  u32 done = 0;
  int complete = 1;
  while (done < len) {
    u32 n = len - done;
    if (n > sizeof(chunk))
      n = (u32)sizeof(chunk);
    int ok = raw ? disc_read_raw(ios->disc, offset + done, n, chunk)
                 : disc_read(ios->disc, offset + done, n, chunk);
    if (!ok)
      complete = 0;
    write_bytes(cpu, out + done, chunk, n);
    done += n;
  }
  ios->di_reads++;
  ios->di_read_bytes += len;
  if (!complete)
    ios->di_misses++;
  return DI_SUCCESS;
}

static s32 handle_di(Ios *ios, CPUState *cpu, u32 request, u32 in, u32 in_size,
                     u32 out, u32 out_size) {
  (void)in_size;
  u32 len = in ? mem_read32(cpu, in + 4u) : 0;
  u32 pos = in ? mem_read32(cpu, in + 8u) : 0;

  switch (request) {
  case DVDLowReadDiskID:
    if (!out || out_size < 0x20u)
      return DI_SECURITY_ERROR;
    return di_copy(ios, cpu, 0, 0x20u, out, out_size, 1);
  case DVDLowRead:
    /* Partition-relative; the command word holds offset >> 2. */
    ios->di_error = DI_ERR_NONE;
    return di_copy(ios, cpu, (u64)pos << 2, len, out, out_size, 0);
  case DVDLowUnencryptedRead: {
    /* Only the system area is readable unencrypted. The two high ranges are
     * the "error 001" probe: a genuine drive fails them, and so do we. */
    u32 end = pos + (len >> 2);
    if (pos <= 0x14000u && end <= 0x14000u) {
      ios->di_error = DI_ERR_NONE;
      return di_copy(ios, cpu, (u64)pos << 2, len, out, out_size, 1);
    }
    if ((pos >= 0x460A0000u && end <= 0x460A0008u) ||
        (pos >= 0x7ED40000u && end <= 0x7ED40008u)) {
      /* The error-001 probe. IOS passes the address to a genuine drive, which
       * rejects it as out of bounds; the game then reads the drive error and
       * insists on seeing exactly 0x052100. */
      ios->di_error = DI_ERR_BLOCK_OOB;
      return DI_DRIVE_ERROR;
    }
    /* Outside every legal window IOS refuses before the drive sees it, so the
     * drive error is left untouched. */
    return DI_SECURITY_ERROR;
  }
  case DVDLowReportKey:
    /* Retail drives have no key to report; the game uses the refusal as its
     * second "am I on real hardware" probe. */
    ios->di_error = DI_ERR_INVALID_COMMAND;
    return DI_DRIVE_ERROR;
  case DVDLowInquiry:
    if (out) {
      u8 inq[32];
      memset(inq, 0, sizeof(inq));
      memcpy(inq + 16, "MATSHITA", 8);
      memcpy(inq + 24, "DVD-ROM", 7);
      write_bytes(cpu, out, inq, out_size < 32u ? out_size : 32u);
    }
    return DI_SUCCESS;
  case DVDLowGetCoverStatus:
    /* 1 = no disc, 2 = disc inserted. */
    if (!out || out_size < 4u)
      return DI_SECURITY_ERROR;
    mem_write32(cpu, out, 2u);
    return DI_SUCCESS;
  case DVDLowGetCoverRegister:
    if (!out || out_size < 4u)
      return DI_SECURITY_ERROR;
    mem_write32(cpu, out, 0u); /* cover closed */
    return DI_SUCCESS;
  case DVDLowGetLength:
    if (!out || out_size < 4u)
      return DI_SECURITY_ERROR;
    mem_write32(cpu, out, ios->di_read_bytes ? len : 0u);
    return DI_SUCCESS;
  case DVDLowGetImmBuf:
    if (!out || out_size < 4u)
      return DI_SECURITY_ERROR;
    mem_write32(cpu, out, 0u);
    return DI_SUCCESS;
  case DVDLowWaitForCoverClose:
    return DI_COVER_CLOSED;
  case DVDLowOpenPartition:
    /* Only valid as an ioctlv. */
    return DI_SECURITY_ERROR;
  case DVDLowClosePartition:
    ios->partition_open = 0;
    return DI_SUCCESS;
  case DVDLowSeek:
  case DVDLowReset:
  case DVDLowNotifyReset:
  case DVDLowClearCoverInterrupt:
  case DVDLowMaskCoverInterrupt:
  case DVDLowUnmaskCoverInterrupt:
  case DVDLowUnmaskStatusInterrupts:
  case DVDLowStopMotor:
    return DI_SUCCESS;
  case DVDLowRequestError:
    if (!out || out_size < 4u)
      return DI_SECURITY_ERROR;
    mem_write32(cpu, out, ios->di_error);
    return DI_SUCCESS;
  default:
    return DI_SUCCESS;
  }
}

/* ---- /dev/fs (ISFS) ---------------------------------------------------- */

static s32 fs_handle(Ios *ios, u32 fd) {
  if (fd >= IOS_FD_MAX)
    return -1;
  return ios->fd_handle[fd];
}

static s32 handle_fs(Ios *ios, CPUState *cpu, u32 fd, u32 request, u32 in,
                     u32 in_size, u32 out, u32 out_size) {
  char path[NAND_PATH_MAX], path2[NAND_PATH_MAX];
  if (!ios->nand)
    return NAND_ENOENT;

  switch (request) {
  case ISFS_IOCTL_GETSTATS: {
    /* A plausible 512 MiB NAND. Titles only sanity-check these. */
    static const u32 stats[7] = {0x4000u, 0x00007CCEu, 0x00000FB0u, 0u,
                                 0x0000000Au, 0x0000146Bu, 0x00000100u};
    if (!out || out_size < sizeof(stats))
      return NAND_EINVAL;
    for (u32 i = 0; i < 7; i++)
      mem_write32(cpu, out + i * 4u, stats[i]);
    return NAND_OK;
  }
  case ISFS_IOCTL_CREATEDIR:
    if (!in || in_size < ISFS_PARAMS_SIZE)
      return NAND_EINVAL;
    read_str(cpu, in + 6u, path, sizeof(path));
    return nand_create_dir(ios->nand, path);
  case ISFS_IOCTL_CREATEFILE:
    if (!in || in_size < ISFS_PARAMS_SIZE)
      return NAND_EINVAL;
    read_str(cpu, in + 6u, path, sizeof(path));
    return nand_create_file(ios->nand, path);
  case ISFS_IOCTL_SETATTR:
  case ISFS_IOCTL_SETFILEVERCTRL:
  case ISFS_IOCTL_SHUTDOWN:
    return NAND_OK;
  case ISFS_IOCTL_GETATTR: {
    if (!in || in_size < 64u || !out || out_size < ISFS_PARAMS_SIZE)
      return NAND_EINVAL;
    read_str(cpu, in, path, sizeof(path));
    s32 kind = nand_exists(ios->nand, path);
    if (kind < 0)
      return kind;
    /* uid 0, gid 0, path echoed, rw/rw/rw, attribute 0. */
    for (u32 i = 0; i < ISFS_PARAMS_SIZE; i++)
      mem_write8(cpu, out + i, 0);
    write_str(cpu, out + 6u, path);
    mem_write8(cpu, out + 70u, 3);
    mem_write8(cpu, out + 71u, 3);
    mem_write8(cpu, out + 72u, 3);
    return NAND_OK;
  }
  case ISFS_IOCTL_DELETE:
    if (!in || in_size < 64u)
      return NAND_EINVAL;
    read_str(cpu, in, path, sizeof(path));
    return nand_delete(ios->nand, path);
  case ISFS_IOCTL_RENAME:
    if (!in || in_size < 128u)
      return NAND_EINVAL;
    read_str(cpu, in, path, sizeof(path));
    read_str(cpu, in + 64u, path2, sizeof(path2));
    return nand_rename(ios->nand, path, path2);
  case ISFS_IOCTL_GETFILESTATS: {
    /* Issued on the file's own fd, not on /dev/fs. */
    s32 h = fs_handle(ios, fd);
    u32 size = 0, at = 0;
    if (h < 0)
      return NAND_EINVAL;
    if (!out || out_size < 8u)
      return NAND_EINVAL;
    s32 r = nand_file_stats(ios->nand, h, &size, &at);
    if (r < 0)
      return r;
    mem_write32(cpu, out, size);
    mem_write32(cpu, out + 4u, at);
    return NAND_OK;
  }
  case ISFS_IOCTL_FORMAT:
    return NAND_EACCES;
  default:
    return NAND_EINVAL;
  }
}

/* ioctlv vectors are {u32 address, u32 length} pairs. */
static u32 vec_addr(CPUState *cpu, u32 vecs, u32 i) {
  return vecs ? to_ea(mem_read32(cpu, vecs + i * 8u)) : 0u;
}

static u32 vec_size(CPUState *cpu, u32 vecs, u32 i) {
  return vecs ? mem_read32(cpu, vecs + i * 8u + 4u) : 0u;
}

/* ES ioctlvs. Only the two the boot path actually needs are real; the SDK
 * reads the output buffers whatever we return, so leaving them untouched hands
 * the game uninitialised stack. That is what drove OSPlayTime.c:775 "Expired":
 * a garbage ticket view looked like an expired play-time limit. */
static s32 handle_es_v(Ios *ios, CPUState *cpu, u32 request, u32 vecs,
                       u32 argc_in, u32 argc_io) {
  (void)ios;
  switch (request) {
  case IOCTL_ES_DIGETTICKETVIEW: {
    if (argc_io < 1u)
      return IPC_EINVAL;
    u32 out = vec_addr(cpu, vecs, argc_in);
    u32 size = vec_size(cpu, vecs, argc_in);
    if (!out || size < ES_TICKET_VIEW_SIZE)
      return IPC_EINVAL;
    u8 view[ES_TICKET_VIEW_SIZE];
    memset(view, 0, sizeof(view));
    /* version 0, no device lock, no time limits; title id at +0x0D. */
    view[0x0D] = (u8)(TITLE_HI >> 24);
    view[0x0E] = (u8)(TITLE_HI >> 16);
    view[0x0F] = (u8)(TITLE_HI >> 8);
    view[0x10] = (u8)TITLE_HI;
    view[0x11] = (u8)(TITLE_LO >> 24);
    view[0x12] = (u8)(TITLE_LO >> 16);
    view[0x13] = (u8)(TITLE_LO >> 8);
    view[0x14] = (u8)TITLE_LO;
    view[0x15] = 0xFF; /* access_mask */
    view[0x16] = 0xFF;
    memset(view + 0x51, 0xFF, 0x40); /* every content readable */
    write_bytes(cpu, out, view, ES_TICKET_VIEW_SIZE);
    return IPC_SUCCESS;
  }
  case IOCTL_ES_GETCONSUMPTION: {
    /* io[0] is the limits array, io[1] the count actually written. A retail
     * title has no consumption records, so the count is zero. */
    if (argc_io < 2u)
      return IPC_EINVAL;
    u32 count_out = vec_addr(cpu, vecs, argc_in + 1u);
    if (count_out)
      mem_write32(cpu, count_out, 0u);
    return IPC_SUCCESS;
  }
  default:
    return IPC_SUCCESS;
  }
}

/* /dev/usb/oh1/57e/305 -- the Wii's Bluetooth dongle, as IOS exposes it.
 * ioctlv 0 is a USB control transfer carrying an HCI command, 1 a bulk
 * transfer carrying ACL data and 2 an interrupt transfer carrying HCI events.
 * The bulk and interrupt directions here are *reads*: on real hardware they
 * stay pending inside IOS until the dongle has something to hand back, so they
 * are parked and completed later, out of ios_bt_tick.
 *
 * The declared transfer length is not trusted. The interrupt and bulk vectors
 * carry it big endian while the control vector mirrors USB's own little-endian
 * setup packet, and an HCI command already states its own length, so the
 * packet and the vector size settle it between them. */
static s32 handle_bt_v(Ios *ios, CPUState *cpu, u32 ppcmsg, u32 cmd_id,
                       u32 request, u32 vecs, u32 argc_in, u32 argc_io,
                       int *hang) {
  if (ios->bt_log_left && ios->log) {
    ios->bt_log_left--;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "bt ioctlv req=0x%x in=%u io=%u",
                     (unsigned)request, (unsigned)argc_in, (unsigned)argc_io);
    u32 total = argc_in + argc_io;
    for (u32 i = 0; i < total && i < 8u && n < (int)sizeof(buf) - 8; i++) {
      u32 a = vec_addr(cpu, vecs, i);
      u32 l = vec_size(cpu, vecs, i);
      n += snprintf(buf + n, sizeof(buf) - (size_t)n, " v%u=0x%08x/%u",
                    (unsigned)i, (unsigned)a, (unsigned)l);
      if (i < argc_in && a && l) {
        u32 m = l < 12u ? l : 12u;
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "[");
        for (u32 j = 0; j < m && n < (int)sizeof(buf) - 4; j++)
          n += snprintf(buf + n, sizeof(buf) - (size_t)n, "%02x",
                        (unsigned)mem_read8(cpu, a + j));
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "]");
      }
    }
    ios->log(buf, ios->log_user);
  }

  if (request == BT_IOCTLV_CTRLMSG) {
    if (argc_io < 1u)
      return IPC_EINVAL;
    u32 data = vec_addr(cpu, vecs, argc_in);
    u32 cap = vec_size(cpu, vecs, argc_in);
    if (!data || cap < 3u)
      return 0;
    u8 pkt[BT_PKT_MAX];
    u32 n = cap < BT_PKT_MAX ? cap : BT_PKT_MAX;
    read_bytes(cpu, data, pkt, n);
    u32 want = 3u + pkt[2];
    if (want < n)
      n = want;
    if (ios->bt_cmd_log_left && ios->log) {
      /* The opcode and its parameters, which is what tells one config command
       * from the next when the stack goes quiet mid-sequence. */
      ios->bt_cmd_log_left--;
      char buf[128];
      int k = snprintf(buf, sizeof(buf), "bt hci cmd 0x%04x len=%u:",
                       (unsigned)(pkt[0] | ((u32)pkt[1] << 8)), (unsigned)pkt[2]);
      for (u32 i = 3; i < n && k < (int)sizeof(buf) - 4; i++)
        k += snprintf(buf + k, sizeof(buf) - (size_t)k, " %02x",
                      (unsigned)pkt[i]);
      ios->log(buf, ios->log_user);
    }
    bt_hci_command(&ios->bt, pkt, n);
    return (s32)n;
  }

  if (request == BT_IOCTLV_BLKMSG && argc_io >= 1u) {
    /* Bulk OUT carries ACL data down to the remote; bulk IN is a read. The
     * endpoint byte tells the two apart. */
    u8 endpoint = argc_in >= 1u ? mem_read8(cpu, vec_addr(cpu, vecs, 0)) : 0x82u;
    if (!(endpoint & 0x80u)) {
      u32 data = vec_addr(cpu, vecs, argc_in);
      u32 cap = vec_size(cpu, vecs, argc_in);
      u8 pkt[BT_PKT_MAX];
      u32 n = cap < BT_PKT_MAX ? cap : BT_PKT_MAX;
      if (data && n)
        read_bytes(cpu, data, pkt, n);
      if (ios->bt_cmd_log_left && ios->log) {
        /* ACL down to the remote: L2CAP signalling and HID output reports. */
        ios->bt_cmd_log_left--;
        char buf[160];
        int k = snprintf(buf, sizeof(buf), "bt acl out len=%u:", n);
        for (u32 i = 0; i < n && k < (int)sizeof(buf) - 4; i++)
          k += snprintf(buf + k, sizeof(buf) - (size_t)k, " %02x",
                        (unsigned)pkt[i]);
        ios->log(buf, ios->log_user);
      }
      bt_acl_write(&ios->bt, pkt, n);
      return (s32)n;
    }
  }

  if (argc_io < 1u)
    return IPC_EINVAL;
  if (ios->bt_read_n >= IOS_BT_PENDING) {
    ios->bt_overflow++;
    return IPC_EMAX;
  }
  IosBtRead *r = &ios->bt_read[ios->bt_read_n++];
  r->ppcmsg = ppcmsg;
  r->cmd = cmd_id;
  r->buf = vec_addr(cpu, vecs, argc_in);
  r->cap = vec_size(cpu, vecs, argc_in);
  r->kind = request == BT_IOCTLV_INTRMSG ? IOS_BT_READ_EVENT : IOS_BT_READ_ACL;
  *hang = 1;
  return IPC_SUCCESS;
}

static s32 handle_di_v(Ios *ios, CPUState *cpu, u32 request, u32 vecs,
                       u32 argc_in, u32 argc_io) {
  if (request == DVDLowOpenPartition) {
    if (argc_in != 3u || argc_io != 2u)
      return DI_BAD_ARGUMENT;
    u32 cmd = vec_addr(cpu, vecs, 0);
    u32 tmd_out = vec_addr(cpu, vecs, 3);
    u32 tmd_size = vec_size(cpu, vecs, 3);
    u32 err_out = vec_addr(cpu, vecs, 4);
    u64 part = cmd ? (u64)mem_read32(cpu, cmd + 4u) << 2 : 0;
    ios->partition_open = 1;
    if (tmd_out && ios->disc && ios->disc->tmd) {
      u32 n = ios->disc->tmd_size < tmd_size ? ios->disc->tmd_size : tmd_size;
      write_bytes(cpu, tmd_out, ios->disc->tmd, n);
    }
    if (err_out)
      mem_write32(cpu, err_out, 0u); /* ES_DIVerify succeeded */
    if (ios->log_left && ios->log) {
      ios->log_left--;
      char buf[96];
      snprintf(buf, sizeof(buf), "di open-partition off=0x%llx tmd=0x%x/%u",
               (unsigned long long)part, (unsigned)tmd_out, (unsigned)tmd_size);
      ios->log(buf, ios->log_user);
    }
    return DI_SUCCESS;
  }
  return DI_SECURITY_ERROR;
}

static s32 handle_fs_v(Ios *ios, CPUState *cpu, u32 request, u32 vecs,
                       u32 argc_in, u32 argc_io) {
  char path[NAND_PATH_MAX];
  if (!ios->nand)
    return NAND_ENOENT;

  if (request == ISFS_IOCTLV_READDIR) {
    if (argc_in < 1u || argc_io < 1u || argc_in != argc_io)
      return NAND_EINVAL;
    read_str(cpu, vec_addr(cpu, vecs, 0), path, sizeof(path));
    if (argc_in == 1u) {
      /* Count-only form: the single io vector holds the entry count. */
      u32 count_ea = vec_addr(cpu, vecs, 1);
      u32 total = 0;
      s32 r = nand_read_dir(ios->nand, path, NULL, 0, &total);
      if (r < 0)
        return r;
      if (count_ea)
        mem_write32(cpu, count_ea, total);
      return NAND_OK;
    }
    u32 max_ea = vec_addr(cpu, vecs, 1);
    u32 names_ea = vec_addr(cpu, vecs, 2);
    u32 count_ea = vec_addr(cpu, vecs, 3);
    u32 max = max_ea ? mem_read32(cpu, max_ea) : 0;
    if (max > 64u)
      max = 64u;
    char names[64 * 13];
    u32 total = 0;
    s32 packed = nand_read_dir(ios->nand, path, names, max, &total);
    if (packed < 0)
      return packed;
    if (names_ea)
      write_bytes(cpu, names_ea, names, max * 13u);
    if (count_ea)
      mem_write32(cpu, count_ea, (u32)packed);
    return NAND_OK;
  }

  if (request == ISFS_IOCTLV_GETUSAGE) {
    if (argc_io < 2u)
      return NAND_EINVAL;
    u32 clusters_ea = vec_addr(cpu, vecs, argc_in);
    u32 inodes_ea = vec_addr(cpu, vecs, argc_in + 1u);
    if (clusters_ea)
      mem_write32(cpu, clusters_ea, 0u);
    if (inodes_ea)
      mem_write32(cpu, inodes_ea, 0u);
    return NAND_OK;
  }
  return NAND_EINVAL;
}

static s32 handle_ioctl(Ios *ios, CPUState *cpu, int dev, u32 fd, u32 request,
                        u32 in, u32 in_size, u32 out, u32 out_size, int *hang) {
  ios->ioctls++;
  if (dev >= 0 && dev <= IOS_DEV_OTHER)
    ios->dev_ioctls[dev]++;
  switch (dev) {
  case IOS_DEV_ES:
    return handle_es(cpu, request, out, out_size);
  case IOS_DEV_DI:
    /* The DI command buffer is 0x20 bytes: [0]=cmd<<24, [1]=length,
     * [2]=offset>>2 for DVDLowRead. Log it so a stalled boot shows which
     * disc read it is waiting on. */
    ios->di_cmds++;
    ios->di_last_cmd = request;
    if (ios->di_log_left && ios->log) {
      ios->di_log_left--;
      char buf[160];
      u32 w0 = in ? mem_read32(cpu, in) : 0;
      u32 len = in ? mem_read32(cpu, in + 4u) : 0;
      u32 off = in ? mem_read32(cpu, in + 8u) : 0;
      snprintf(buf, sizeof(buf),
               "di cmd=0x%02x w0=0x%08x len=0x%x off=0x%llx out=0x%08x/0x%x",
               (unsigned)request, (unsigned)w0, (unsigned)len,
               (unsigned long long)((u64)off << 2), (unsigned)out,
               (unsigned)out_size);
      ios->log(buf, ios->log_user);
    }
    return handle_di(ios, cpu, request, in, in_size, out, out_size);
  case IOS_DEV_FS:
    return handle_fs(ios, cpu, fd, request, in, in_size, out, out_size);
  case IOS_DEV_STM_EH:
    if (request == IOCTL_STM_EVENTHOOK) {
      *hang = 1;
      return IPC_SUCCESS;
    }
    return IPC_SUCCESS;
  case IOS_DEV_STM_IMM:
    if (request == IOCTL_STM_SHUTDOWN || request == IOCTL_STM_IDLE ||
        request == IOCTL_STM_HOTRESET)
      return IPC_SUCCESS;
    return IPC_SUCCESS;
  default:
    return IPC_SUCCESS;
  }
}

void ios_init(Ios *ios, IosLogFn log, void *log_user) {
  memset(ios, 0, sizeof(*ios));
  ios->log = log;
  ios->log_user = log_user;
  ios->log_left = 400;
  ios->di_log_left = 48;
  ios->bt_log_left = 64;
  /* Enough to see a stuck reply come and go a few times, not enough for a
   * livelock to fill the log with the same line. */
  ios->stuck_log_left = 16;
  /* Off unless a host asks: the HCI and ACL traffic dump is a debugging tool,
   * not something every run should carry. */
  ios->bt_cmd_log_left = 0;
  bt_init(&ios->bt);
  for (u32 i = 0; i < IOS_FD_MAX; i++)
    ios->fd_handle[i] = -1;
}

void ios_attach(Ios *ios, struct Disc *disc, struct Nand *nand) {
  ios->disc = disc;
  ios->nand = nand;
}

/* IPC has one reply register and one Y1 line, so a reply can only be posted
 * once the guest has taken the previous one. Everything queues, including the
 * synchronous replies, so ordering is the same either way. */
static void queue_reply(Ios *ios, u32 ppcmsg, u32 cmd, s32 ret) {
  if (ios->rq_tail - ios->rq_head >= IOS_REPLY_MAX) {
    ios->rq_dropped++;
    return;
  }
  IosReply *r = &ios->replyq[ios->rq_tail % IOS_REPLY_MAX];
  r->ppcmsg = ppcmsg;
  r->cmd = cmd;
  r->ret = ret;
  ios->rq_tail++;
}

/* How many consecutive pumps finding the same uncollected reply count as
 * "the guest is not coming for it", and how often to re-assert after that.
 * The first few hundred are the case the level model exists for -- a reply
 * posted while the guest is inside its own handler -- and re-asserting on
 * every pump forever is the case that starves the machine. */
enum { IOS_STUCK_LOG_AT = 256u, IOS_STUCK_THROTTLE = 4096u };

void ios_pump(Ios *ios, Flipper *f, CPUState *cpu) {
  if (ios->rq_head == ios->rq_tail) {
    ios->stuck_pumps = 0;
    return;
  }
  if (f->ipc_ctrl & 4u) {
    /* The guest has not collected the last reply. Re-assert the line in case
     * its handler cleared the flag after acknowledging Y1 -- but not on every
     * pump once it is clear nothing is coming for it. Re-asserting a level the
     * guest keeps acknowledging and never clearing is faithful to the
     * hardware and also a livelock: the CPU spends its whole budget
     * re-entering the IPC handler and never runs a game thread again, which
     * from outside looks like a hang with a busy CPU and a frozen GX FIFO. */
    const IosReply *head = &ios->replyq[ios->rq_head % IOS_REPLY_MAX];
    ios->stuck_ppcmsg = head->ppcmsg;
    ios->stuck_cmd = head->cmd;
    ios->stuck_ret = head->ret;
    /* The request buffer still holds the fd: it is only overwritten when the
     * reply is actually handed over, below. */
    ios->stuck_fd = mem_read32(cpu, to_ea(head->ppcmsg) + 8u);
    ios->stuck_pumps++;
    if (ios->stuck_pumps > ios->stuck_worst)
      ios->stuck_worst = ios->stuck_pumps;
    if (ios->stuck_pumps == IOS_STUCK_LOG_AT && ios->stuck_log_left &&
        ios->log) {
      char buf[160];
      ios->stuck_log_left--;
      snprintf(buf, sizeof(buf),
               "ios reply stuck: ppcmsg=0x%08x cmd=%u fd=%u ret=%d queued=%u",
               ios->stuck_ppcmsg, ios->stuck_cmd, ios->stuck_fd,
               (int)ios->stuck_ret, ios->rq_tail - ios->rq_head);
      ios->log(buf, ios->log_user);
    }
    if (ios->stuck_pumps < IOS_STUCK_THROTTLE ||
        (ios->stuck_pumps % IOS_STUCK_THROTTLE) == 0u)
      ipc_update_irq(f);
    return;
  }
  ios->stuck_pumps = 0;
  IosReply *r = &ios->replyq[ios->rq_head % IOS_REPLY_MAX];
  ios->rq_head++;
  u32 ea = to_ea(r->ppcmsg);
  mem_write32(cpu, ea + 4u, (u32)r->ret);
  mem_write32(cpu, ea + 8u, r->cmd);
  mem_write32(cpu, ea, IPC_REPLY);
  f->ipc_armmsg = r->ppcmsg;
  f->ipc_ctrl |= 4u; /* Y1 reply */
  ipc_update_irq(f);
  ios->replies++;
}

void ios_doorbell(Ios *ios, Flipper *f, CPUState *cpu, u32 ppcmsg) {
  f->ipc_ctrl &= ~1u; /* clear X1 */
  f->ipc_ctrl |= 2u;  /* Y2 ACK */
  f->ipc_x1 = 0;
  ipc_update_irq(f);

  if (!ppcmsg) {
    /* Handshake doorbell with no command buffer: also raise Y1 so
     * IPC waiters that look for a reply (not just ACK) can proceed. */
    f->ipc_ctrl |= 4u;
    f->ipc_armmsg = 0;
    ipc_update_irq(f);
    return;
  }

  u32 ea = to_ea(ppcmsg);
  u32 cmd = mem_read32(cpu, ea);
  u32 fd = mem_read32(cpu, ea + 8);
  s32 ret = IPC_SUCCESS;
  int hang = 0;
  ios->last_cmd = cmd;
  ios->last_ppcmsg = ppcmsg;

  switch (cmd) {
  case IPC_CMD_OPEN: {
    u32 path_raw = mem_read32(cpu, ea + 0xCu);
    u32 path_ea = to_ea(path_raw);
    ios->last_path[0] = 0;
    if (!try_read_path(cpu, path_raw, ios->last_path, IOS_PATH_MAX) &&
        ios->pending_path[0]) {
      /* iosAlloc of the request can dcbz/reuse the IPC bump slot the
       * client just strncpy'd the path into. Use the snapshot from
       * IOS_Open entry (r3) taken before that alloc. */
      memcpy(ios->last_path, ios->pending_path, IOS_PATH_MAX);
      path_ea = to_ea(ios->pending_path_ea);
    }
    int dev = match_dev(ios->last_path);
    u32 mode = mem_read32(cpu, ea + 0x10u);
    ios->opens++;
    if (dev >= 0 && dev <= IOS_DEV_OTHER)
      ios->dev_opens[dev]++;
    if (dev == IOS_DEV_NONE)
      ret = IPC_ENOENT;
    else if (dev == IOS_DEV_FS && strcmp(ios->last_path, "/dev/fs") != 0) {
      /* A NAND file, not the FS device node. */
      ios->fs_opens++;
      s32 h = ios->nand ? nand_file_open(ios->nand, ios->last_path,
                                         (int)(mode & 3u) ? (int)(mode & 3u)
                                                          : NAND_MODE_READ)
                        : NAND_ENOENT;
      if (h < 0) {
        ios->fs_misses++;
        ret = h;
      } else {
        ret = alloc_fd(ios, dev);
        if (ret < 0) {
          nand_file_close(ios->nand, h);
        } else {
          ios->fd_handle[ret] = h;
          mem_write32(cpu, ea + 8, (u32)ret);
        }
      }
    } else {
      ret = alloc_fd(ios, dev);
      if (ret >= 0)
        mem_write32(cpu, ea + 8, (u32)ret);
    }
    if (ios->log_left && ios->log) {
      ios->log_left--;
      char buf[160];
      snprintf(buf, sizeof(buf),
               "ios open '%s' ret=%d path_ea=0x%x raw=0x%x ppcmsg=0x%x",
               ios->last_path[0] ? ios->last_path : "(empty)", (int)ret,
               (unsigned)path_ea, (unsigned)path_raw, (unsigned)ppcmsg);
      ios->log(buf, ios->log_user);
      if (!ios->last_path[0] && ios->log_left) {
        ios->log_left--;
        snprintf(buf, sizeof(buf),
                 "  req %08x %08x %08x %08x %08x %08x %08x %08x",
                 (unsigned)mem_read32(cpu, ea),
                 (unsigned)mem_read32(cpu, ea + 4),
                 (unsigned)mem_read32(cpu, ea + 8),
                 (unsigned)mem_read32(cpu, ea + 12),
                 (unsigned)mem_read32(cpu, ea + 16),
                 (unsigned)mem_read32(cpu, ea + 20),
                 (unsigned)mem_read32(cpu, ea + 24),
                 (unsigned)mem_read32(cpu, ea + 28));
        ios->log(buf, ios->log_user);
      }
    }
    ios->pending_path[0] = 0;
    break;
  }
  case IPC_CMD_CLOSE:
    if (fd < IOS_FD_MAX) {
      if (ios->fd_handle[fd] >= 0 && ios->nand)
        nand_file_close(ios->nand, ios->fd_handle[fd]);
      ios->fd_handle[fd] = -1;
      ios->fd_dev[fd] = IOS_DEV_NONE;
    }
    ret = IPC_SUCCESS;
    break;
  case IPC_CMD_READ: {
    u32 buf = to_ea(mem_read32(cpu, ea + 0xCu));
    u32 len = mem_read32(cpu, ea + 0x10u);
    s32 h = fd < IOS_FD_MAX ? ios->fd_handle[fd] : -1;
    ret = 0;
    if (h >= 0 && ios->nand && buf) {
      u8 chunk[1024];
      u32 done = 0;
      while (done < len) {
        u32 n = len - done;
        if (n > sizeof(chunk))
          n = (u32)sizeof(chunk);
        s32 got = nand_file_read(ios->nand, h, chunk, n);
        if (got <= 0) {
          if (got < 0 && done == 0)
            ret = got;
          break;
        }
        write_bytes(cpu, buf + done, chunk, (u32)got);
        done += (u32)got;
        if ((u32)got < n)
          break;
      }
      if (ret >= 0)
        ret = (s32)done;
      ios->fs_reads++;
      ios->fs_read_bytes += done;
    }
    break;
  }
  case IPC_CMD_WRITE: {
    u32 buf = to_ea(mem_read32(cpu, ea + 0xCu));
    u32 len = mem_read32(cpu, ea + 0x10u);
    s32 h = fd < IOS_FD_MAX ? ios->fd_handle[fd] : -1;
    ret = 0;
    if (h >= 0 && ios->nand && buf) {
      u8 chunk[1024];
      u32 done = 0;
      while (done < len) {
        u32 n = len - done;
        if (n > sizeof(chunk))
          n = (u32)sizeof(chunk);
        read_bytes(cpu, buf + done, chunk, n);
        s32 put = nand_file_write(ios->nand, h, chunk, n);
        if (put <= 0) {
          if (put < 0 && done == 0)
            ret = put;
          break;
        }
        done += (u32)put;
        if ((u32)put < n)
          break;
      }
      if (ret >= 0)
        ret = (s32)done;
    }
    break;
  }
  case IPC_CMD_SEEK: {
    s32 where = (s32)mem_read32(cpu, ea + 0xCu);
    u32 whence = mem_read32(cpu, ea + 0x10u);
    s32 h = fd < IOS_FD_MAX ? ios->fd_handle[fd] : -1;
    ret = h >= 0 && ios->nand ? nand_file_seek(ios->nand, h, where, whence) : 0;
    break;
  }
  case IPC_CMD_IOCTL: {
    u32 request = mem_read32(cpu, ea + 0xCu);
    u32 in = to_ea(mem_read32(cpu, ea + 0x10u));
    u32 in_size = mem_read32(cpu, ea + 0x14u);
    u32 out = to_ea(mem_read32(cpu, ea + 0x18u));
    u32 out_size = mem_read32(cpu, ea + 0x1Cu);
    int dev = fd_dev(ios, fd);
    ret = handle_ioctl(ios, cpu, dev, fd, request, in, in_size, out, out_size,
                       &hang);
    if (ios->log_left && ios->log) {
      ios->log_left--;
      char buf[96];
      snprintf(buf, sizeof(buf), "ios ioctl fd=%u req=0x%x ret=%d",
               (unsigned)fd, (unsigned)request, (int)ret);
      ios->log(buf, ios->log_user);
    }
    break;
  }
  case IPC_CMD_IOCTLV: {
    u32 request = mem_read32(cpu, ea + 0xCu);
    u32 argc_in = mem_read32(cpu, ea + 0x10u);
    u32 argc_io = mem_read32(cpu, ea + 0x14u);
    u32 vecs = to_ea(mem_read32(cpu, ea + 0x18u));
    int dev = fd_dev(ios, fd);
    ios->ioctlvs++;
    if (dev >= 0 && dev <= IOS_DEV_OTHER)
      ios->dev_ioctlvs[dev]++;
    if (dev == IOS_DEV_DI)
      ret = handle_di_v(ios, cpu, request, vecs, argc_in, argc_io);
    else if (dev == IOS_DEV_FS)
      ret = handle_fs_v(ios, cpu, request, vecs, argc_in, argc_io);
    else if (dev == IOS_DEV_ES)
      ret = handle_es_v(ios, cpu, request, vecs, argc_in, argc_io);
    else if (dev == IOS_DEV_BT)
      ret = handle_bt_v(ios, cpu, ppcmsg, cmd, request, vecs, argc_in, argc_io,
                        &hang);
    else
      ret = IPC_SUCCESS;
    if (ios->log_left && ios->log) {
      ios->log_left--;
      char buf[96];
      snprintf(buf, sizeof(buf), "ios ioctlv fd=%u req=0x%x ret=%d",
               (unsigned)fd, (unsigned)request, (int)ret);
      ios->log(buf, ios->log_user);
    }
    break;
  }
  default:
    ret = IPC_EINVAL;
    break;
  }

  ios->last_ret = ret;
  if (hang) {
    ios->hangs++;
    return;
  }
  queue_reply(ios, ppcmsg, cmd, ret);
  ios_pump(ios, f, cpu);
}

void ios_bt_service(Ios *ios, Flipper *f, CPUState *cpu) {
  /* Hand each parked USB read the first packet its endpoint has. A read that
   * finds nothing stays parked, which is what keeps the stack's receive
   * pipeline armed instead of spinning. */
  u32 kept = 0;
  for (u32 i = 0; i < ios->bt_read_n; i++) {
    IosBtRead *r = &ios->bt_read[i];
    u8 pkt[BT_PKT_MAX];
    u32 cap = r->cap < BT_PKT_MAX ? r->cap : BT_PKT_MAX;
    u32 n = r->kind == IOS_BT_READ_EVENT ? bt_pop_event(&ios->bt, pkt, cap)
                                         : bt_pop_acl(&ios->bt, pkt, cap);
    if (!n) {
      ios->bt_read[kept++] = *r;
      continue;
    }
    if (r->kind != IOS_BT_READ_EVENT && ios->bt_cmd_log_left && ios->log) {
      ios->bt_cmd_log_left--;
      char buf[160];
      int k = snprintf(buf, sizeof(buf), "bt acl in  len=%u:", n);
      for (u32 i = 0; i < n && k < (int)sizeof(buf) - 4; i++)
        k += snprintf(buf + k, sizeof(buf) - (size_t)k, " %02x",
                      (unsigned)pkt[i]);
      ios->log(buf, ios->log_user);
    }
    write_bytes(cpu, r->buf, pkt, n);
    queue_reply(ios, r->ppcmsg, r->cmd, (s32)n);
    ios->hangs--;
  }
  ios->bt_read_n = kept;
  ios_pump(ios, f, cpu);
}

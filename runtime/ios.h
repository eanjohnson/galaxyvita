#ifndef GALAXYVITA_IOS_H
#define GALAXYVITA_IOS_H

#include "bt.h"
#include "common/types.h"

struct CPUState;
struct Flipper;
struct Disc;
struct Nand;

enum {
  IOS_FD_MAX = 24,
  IOS_PATH_MAX = 64,
  /* IPC has one reply register, so replies the guest has not collected yet
   * have to wait their turn. */
  IOS_REPLY_MAX = 32,
  /* USB reads the Bluetooth stack leaves posted while it waits for the dongle
   * to have something to say. */
  IOS_BT_PENDING = 8
};

/* A reply the guest has not been handed yet. */
typedef struct IosReply {
  u32 ppcmsg;
  u32 cmd;
  s32 ret;
} IosReply;

/* A USB read request held open until the Bluetooth controller has a packet
 * for it. `kind` is the endpoint it is waiting on. */
typedef struct IosBtRead {
  u32 ppcmsg;
  u32 cmd;
  u32 buf;
  u32 cap;
  int kind;
} IosBtRead;

enum { IOS_BT_READ_EVENT = 1, IOS_BT_READ_ACL = 2 };

enum {
  IOS_DEV_NONE = 0,
  IOS_DEV_ES = 1,
  IOS_DEV_FS = 2,
  IOS_DEV_DI = 3,
  IOS_DEV_STM_IMM = 4,
  IOS_DEV_STM_EH = 5,
  IOS_DEV_OH1 = 6,
  IOS_DEV_BT = 7,
  IOS_DEV_NET = 8,
  IOS_DEV_SDIO = 9,
  IOS_DEV_USB = 10,
  IOS_DEV_AES = 11,
  IOS_DEV_SHA = 12,
  IOS_DEV_OTHER = 13
};

typedef void (*IosLogFn)(const char *msg, void *user);

typedef struct Ios {
  s32 fd_dev[IOS_FD_MAX];
  /* NAND handle behind an /dev/fs file descriptor, or -1 for a device fd. */
  s32 fd_handle[IOS_FD_MAX];
  u32 opens;
  u32 ioctls;
  u32 ioctlvs;
  u32 replies;
  u32 hangs;
  u32 last_cmd;
  s32 last_ret;
  u32 last_ppcmsg;
  char last_path[IOS_PATH_MAX];
  /* Path captured at IOS_Open entry (r3) before iosAlloc can wipe the
   * IPC bump buffer the DVD/NET clients copy into. */
  char pending_path[IOS_PATH_MAX];
  u32 pending_path_ea;
  /* Per-device command census: which HLE device the guest actually leans on. */
  u32 dev_opens[IOS_DEV_OTHER + 1];
  u32 dev_ioctls[IOS_DEV_OTHER + 1];
  u32 dev_ioctlvs[IOS_DEV_OTHER + 1];
  u32 di_cmds;
  u32 di_last_cmd;
  u32 di_log_left;
  u32 bt_log_left;
  u32 bt_cmd_log_left;
  /* Backends. NULL means the old zero-stub behaviour. */
  struct Disc *disc;
  struct Nand *nand;
  int partition_open;
  /* Last drive error, reported by DVDLowRequestError. RMGE01's anti-modchip
   * check demands 0x053100 (invalid address) after the out-of-range probe. */
  u32 di_error;
  u32 di_reads;
  u32 di_read_bytes;
  u32 di_misses;
  u32 fs_opens;
  u32 fs_misses;
  u32 fs_reads;
  u32 fs_read_bytes;
  /* The Bluetooth dongle, with the one synced Wii Remote behind it. */
  Bt bt;
  IosBtRead bt_read[IOS_BT_PENDING];
  u32 bt_read_n;
  u32 bt_overflow;
  IosReply replyq[IOS_REPLY_MAX];
  u32 rq_head;
  u32 rq_tail;
  u32 rq_dropped;
  IosLogFn log;
  void *log_user;
  u32 log_left;
  /* The reply nobody collected.
   *
   * IPC is a level, so a reply the guest never takes re-raises the interrupt
   * for as long as it sits there -- which is what the hardware does, and which
   * turns one stuck reply into a livelock that starves every other thread.
   * Counting the consecutive pumps that found it still there says how stuck it
   * is, and the head reply's own fields say what it was. */
  u32 stuck_pumps;
  u32 stuck_worst;
  u32 stuck_ppcmsg, stuck_cmd, stuck_fd;
  s32 stuck_ret;
  u32 stuck_log_left;
} Ios;

void ios_init(Ios *ios, IosLogFn log, void *log_user);
/* Attach the disc and NAND backends. Either may be NULL. */
void ios_attach(Ios *ios, struct Disc *disc, struct Nand *nand);
/* Snapshot the path argument of Broadway IOS_Open / IOS_OpenAsync. */
void ios_note_open_arg(Ios *ios, struct CPUState *cpu, u32 path_ea);
/* Handle a PPC X1 doorbell. Sets ACK (Y2) always; sets REPLY (Y1) unless the
 * request is a hanging eventhook. */
void ios_doorbell(Ios *ios, struct Flipper *f, struct CPUState *cpu, u32 ppcmsg);
/* Hand the guest the next queued reply, if it has collected the last one.
 * Called from the doorbell and again whenever the guest clears Y1. */
void ios_pump(Ios *ios, struct Flipper *f, struct CPUState *cpu);
/* Complete any parked Bluetooth USB read the controller can now satisfy. */
void ios_bt_service(Ios *ios, struct Flipper *f, struct CPUState *cpu);

#endif

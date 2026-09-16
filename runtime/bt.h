#ifndef GALAXYVITA_BT_H
#define GALAXYVITA_BT_H

#include "common/types.h"

/* The Wii's Bluetooth dongle, modelled at the HCI level, with one Wii Remote
 * permanently synced to it.
 *
 * SMG reaches the player only through WPAD/KPAD, and those sit on top of WUD,
 * the SDK's own Bluetooth host stack, which drives /dev/usb/oh1/57e/305
 * directly. HLE'ing WPAD would mean answering dozens of SDK entry points and
 * would leave the game's own input smoothing, calibration and pointer maths
 * unrun. Speaking HCI instead keeps every line of the game's input code on the
 * recompiled path: the Vita's sticks and buttons are turned into Wii Remote HID
 * reports here, and WUD believes a remote is on the air.
 *
 * Only what the SDK actually asks for is implemented, which is a small corner
 * of Bluetooth: no inquiry (the remote is presented as already synced, through
 * the dongle's stored link keys), no SCO, no encryption above L2CAP. */

enum {
  /* An ACL packet the SDK will accept; its own receive buffer is 1760 bytes. */
  BT_PKT_MAX = 384,
  BT_QUEUE = 64,
  BT_EEPROM_SIZE = 0x1700,
  /* 200 Hz ticks between the console enabling page scan and the remote
   * answering -- about a tenth of a second, the same order as a real one. */
  BT_PAGE_DELAY = 20,
  /* 200 Hz ticks between the ACL link coming up and the remote opening its
   * first HID channel. A real remote does not wait for the console to ask. */
  BT_L2CAP_DELAY = 1
};

typedef struct BtPacket {
  u16 len;
  u8 data[BT_PKT_MAX];
} BtPacket;

typedef struct BtQueue {
  BtPacket q[BT_QUEUE];
  u32 head;
  u32 tail;
  u32 dropped;
} BtQueue;

/* Buttons in the Wii Remote's own core-report bit order, so a report is just
 * the mask byte-swapped into place. */
enum {
  BT_BTN_LEFT = 0x0001u,
  BT_BTN_RIGHT = 0x0002u,
  BT_BTN_DOWN = 0x0004u,
  BT_BTN_UP = 0x0008u,
  BT_BTN_PLUS = 0x0010u,
  BT_BTN_TWO = 0x0100u,
  BT_BTN_ONE = 0x0200u,
  BT_BTN_B = 0x0400u,
  BT_BTN_A = 0x0800u,
  BT_BTN_MINUS = 0x1000u,
  BT_BTN_HOME = 0x8000u
};

/* Nunchuk buttons, in the extension byte's (inverted) bit order. */
enum { BT_NUN_Z = 0x01u, BT_NUN_C = 0x02u };

typedef struct BtInput {
  u16 buttons;
  u8 nunchuk;
  /* Pointer in IR camera units: 0..1023 across, 0..767 down. Negative x parks
   * the pointer off-screen, which is how the sensor bar being out of view
   * looks to the game. */
  s16 ir_x;
  s16 ir_y;
  /* Accelerometers in report units; 0x80 is zero g and 0x9A is one g, matching
   * the calibration block the remote reports out of its EEPROM. */
  u8 acc[3];
  u8 nunchuk_acc[3];
  /* Nunchuk stick, 0x20..0xE0 with 0x80 centred. */
  u8 stick_x;
  u8 stick_y;
} BtInput;

typedef struct Bt {
  BtQueue events;  /* controller -> host, HCI events */
  BtQueue acl_in;  /* controller -> host, ACL data */
  u8 bdaddr[6];
  u8 wm_bdaddr[6];
  u8 link_key[16];
  int connected;
  u16 handle;
  /* Page-scan state, and the 200 Hz countdown to the remote paging us. A real
   * remote only connects once the console is listening, and the console only
   * listens after WUD has brought BTM up; arming this from the scan-enable
   * write is what stands in for the player pressing a button. */
  u8 scan_enable;
  u32 page_in;
  /* L2CAP: `host` is the channel id the SDK chose, `dev` the one we handed
   * back. Data we send is addressed to the host id. */
  u16 cid_ctrl_host;
  u16 cid_ctrl_dev;
  u16 cid_intr_host;
  u16 cid_intr_dev;
  int intr_up;
  /* The remote, not the console, opens the HID channels: once the ACL link is
   * up a Wii Remote connects to PSM 0x11 and then 0x13 itself, and the SDK's
   * HID host only ever answers. `l2c_in` counts down to the first request,
   * `l2c_stage` tracks the handshake: 0 idle, 1 control requested, 3 interrupt
   * requested, 4 both open. */
  u32 l2c_in;
  u8 l2c_stage;
  /* Which channels have already had a configuration request of our own: bit 0
   * control, bit 1 interrupt. One per channel, and only one. */
  u8 cfg_req_sent;
  u8 next_cid;
  u8 sig_id;
  /* HID state. */
  u8 report_mode;
  int continuous;
  int rumble;
  u8 leds;
  int ir_on;
  int speaker_on;
  int ext_enabled;
  u8 eeprom[BT_EEPROM_SIZE];
  /* Extension register file; only the identifier and calibration tail of the
   * 0x04A400xx page is ever read. */
  u8 ext_regs[0x100];
  BtInput input;
  /* Counters, for the run report. */
  u32 hci_cmds;
  u32 hci_unknown;
  u32 events_out;
  u32 acl_out;
  u32 acl_in_n;
  u32 reports;
  u32 last_out_report;
  /* The first HCI opcodes the stack sends, in order: the shape of its init
   * sequence is the quickest way to see how far it got. */
  u16 cmd_log[48];
  u32 cmd_log_n;
  /* Bitmask of output report ids the game has sent, for `bt` in the run
   * report: which parts of the remote WPAD actually drives. */
  u32 out_seen;
} Bt;

void bt_init(Bt *bt);
/* An HCI command packet from the host: opcode (2, little endian), parameter
 * length, parameters. */
void bt_hci_command(Bt *bt, const u8 *cmd, u32 len);
/* An ACL packet from the host, header included. */
void bt_acl_write(Bt *bt, const u8 *pkt, u32 len);
/* Called once per emulated frame: pushes an input report if the remote is in
 * continuous reporting mode. */
void bt_tick(Bt *bt);
/* Pop one queued packet. Returns its length, or 0 when the queue is empty. */
u32 bt_pop_event(Bt *bt, u8 *out, u32 cap);
u32 bt_pop_acl(Bt *bt, u8 *out, u32 cap);

#endif

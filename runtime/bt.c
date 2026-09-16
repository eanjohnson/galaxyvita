#include "bt.h"

#include <string.h>

/* HCI is little endian throughout; the Wii Remote's own HID payloads are big
 * endian. Both appear here, so the accessors are named for which they are. */
static u16 rd16le(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static void wr16le(u8 *p, u16 v) {
  p[0] = (u8)v;
  p[1] = (u8)(v >> 8);
}

static u16 rd16be(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }

static void wr16be(u8 *p, u16 v) {
  p[0] = (u8)(v >> 8);
  p[1] = (u8)v;
}

static void queue_push(BtQueue *q, const u8 *data, u32 len) {
  if (len > BT_PKT_MAX || q->tail - q->head >= BT_QUEUE) {
    q->dropped++;
    return;
  }
  BtPacket *p = &q->q[q->tail % BT_QUEUE];
  p->len = (u16)len;
  memcpy(p->data, data, len);
  q->tail++;
}

static u32 queue_pop(BtQueue *q, u8 *out, u32 cap) {
  if (q->head == q->tail)
    return 0;
  BtPacket *p = &q->q[q->head % BT_QUEUE];
  u32 n = p->len < cap ? p->len : cap;
  memcpy(out, p->data, n);
  q->head++;
  return n;
}

u32 bt_pop_event(Bt *bt, u8 *out, u32 cap) {
  return queue_pop(&bt->events, out, cap);
}

u32 bt_pop_acl(Bt *bt, u8 *out, u32 cap) {
  u32 n = queue_pop(&bt->acl_in, out, cap);
  if (n)
    bt->acl_out++;
  return n;
}

/* ---- HCI events ---------------------------------------------------------- */

#define EVT_INQUIRY_COMPLETE 0x01u
#define EVT_CONNECTION_COMPLETE 0x03u
#define EVT_CONNECTION_REQUEST 0x04u
#define EVT_DISCONNECTION_COMPLETE 0x05u
#define EVT_AUTH_COMPLETE 0x06u
#define EVT_REMOTE_NAME_COMPLETE 0x07u
#define EVT_ENCRYPTION_CHANGE 0x08u
#define EVT_REMOTE_FEATURES 0x0Bu
#define EVT_REMOTE_VERSION 0x0Cu
#define EVT_RETURN_LINK_KEYS 0x15u
#define EVT_COMMAND_COMPLETE 0x0Eu
#define EVT_COMMAND_STATUS 0x0Fu
#define EVT_ROLE_CHANGE 0x12u
#define EVT_NUM_COMPLETED_PACKETS 0x13u
#define EVT_MODE_CHANGE 0x14u
#define EVT_LINK_KEY_NOTIFICATION 0x18u
#define EVT_MAX_SLOTS_CHANGE 0x1Bu
#define EVT_READ_CLOCK_OFFSET 0x1Cu
#define EVT_CONN_PACKET_TYPE_CHANGED 0x1Du

static void event_push(Bt *bt, u8 code, const u8 *params, u32 len) {
  u8 pkt[BT_PKT_MAX];
  if (len + 2u > BT_PKT_MAX)
    return;
  pkt[0] = code;
  pkt[1] = (u8)len;
  if (len)
    memcpy(pkt + 2, params, len);
  queue_push(&bt->events, pkt, len + 2u);
  bt->events_out++;
}

/* Command Complete carries the command's own return parameters, always with
 * status first; every command in this model succeeds. */
static void cmd_complete(Bt *bt, u16 opcode, const u8 *ret, u32 len) {
  u8 p[BT_PKT_MAX];
  if (len + 4u > BT_PKT_MAX)
    return;
  p[0] = 1; /* the host may send one more command */
  wr16le(p + 1, opcode);
  p[3] = 0; /* status: success */
  if (len)
    memcpy(p + 4, ret, len);
  event_push(bt, EVT_COMMAND_COMPLETE, p, len + 4u);
}

static void cmd_complete_status(Bt *bt, u16 opcode) {
  cmd_complete(bt, opcode, NULL, 0);
}

/* Command Status means "accepted, the result follows as its own event". */
static void cmd_status(Bt *bt, u16 opcode) {
  u8 p[4];
  p[0] = 0;
  p[1] = 1;
  wr16le(p + 2, opcode);
  event_push(bt, EVT_COMMAND_STATUS, p, 4);
}

/* ---- The Wii Remote ------------------------------------------------------ */

/* An accelerometer calibration block as the remote stores it: the zero-g and
 * one-g readings for the three axes, their low bits packed into a shared byte,
 * and a checksum the SDK verifies. Written at both 0x0016 and 0x0020, which is
 * where WPAD looks for it. */
static void write_accel_cal(u8 *p) {
  static const u8 zero = 0x80u, one = 0x9Au;
  p[0] = zero;
  p[1] = zero;
  p[2] = zero;
  p[3] = 0;
  p[4] = one;
  p[5] = one;
  p[6] = one;
  p[7] = 0;
  p[8] = 0; /* volume */
  u8 sum = 0x55u;
  for (u32 i = 0; i < 9; i++)
    sum = (u8)(sum + p[i]);
  p[9] = sum;
}

void bt_init(Bt *bt) {
  memset(bt, 0, sizeof(*bt));
  /* Addresses are little endian here, the way HCI carries them. The remote's
   * has to match the one scripts/make_nand.py registers in SYSCONF's BT.DINF,
   * because that list is what the stack pages at boot. */
  static const u8 host[6] = {0x00u, 0x00u, 0x00u, 0x1Du, 0x19u, 0x00u};
  static const u8 remote[6] = {0x01u, 0x00u, 0x00u, 0x1Du, 0x19u, 0x00u};
  memcpy(bt->bdaddr, host, 6);
  memcpy(bt->wm_bdaddr, remote, 6);
  memset(bt->link_key, 0x5Au, 16);
  bt->handle = 0x0100u;
  /* Channel ids the remote hands out. Any value above the 0x0040 floor works;
   * 0x70 keeps them clear of the ones the SDK allocates, so a packet capture
   * says which end a channel id belongs to. */
  bt->next_cid = 0x70u;
  bt->report_mode = 0x30u;
  write_accel_cal(bt->eeprom + 0x16);
  write_accel_cal(bt->eeprom + 0x20);
  /* Extension identifier, read from 0x04A400FA. All zeroes means "nothing
   * plugged in", which is what an unencrypted read of an empty port returns. */
  memset(bt->ext_regs, 0, sizeof(bt->ext_regs));
  bt->input.acc[0] = 0x80u;
  bt->input.acc[1] = 0x80u;
  /* At rest the remote is held level, so gravity is all on Z. */
  bt->input.acc[2] = 0x9Au;
  bt->input.stick_x = 0x80u;
  bt->input.stick_y = 0x80u;
  bt->input.ir_x = -1;
  bt->input.ir_y = -1;
}

/* ---- HCI command dispatch ------------------------------------------------ */

#define OP(ogf, ocf) (u16)(((ogf) << 10) | (ocf))

#define HCI_INQUIRY OP(1, 0x0001)
#define HCI_INQUIRY_CANCEL OP(1, 0x0002)
#define HCI_CREATE_CONNECTION OP(1, 0x0005)
#define HCI_DISCONNECT OP(1, 0x0006)
#define HCI_ACCEPT_CONNECTION OP(1, 0x0009)
#define HCI_LINK_KEY_REPLY OP(1, 0x000B)
#define HCI_LINK_KEY_NEG_REPLY OP(1, 0x000C)
#define HCI_AUTH_REQUESTED OP(1, 0x0011)
#define HCI_SET_CONN_ENCRYPTION OP(1, 0x0013)
#define HCI_REMOTE_NAME_REQUEST OP(1, 0x0019)
#define HCI_READ_REMOTE_FEATURES OP(1, 0x001B)
#define HCI_READ_REMOTE_VERSION OP(1, 0x001D)
#define HCI_CHANGE_PACKET_TYPE OP(1, 0x000F)
#define HCI_READ_CLOCK_OFFSET OP(1, 0x001F)

#define HCI_SNIFF_MODE OP(2, 0x0003)
#define HCI_EXIT_SNIFF_MODE OP(2, 0x0004)
#define HCI_WRITE_LINK_POLICY OP(2, 0x000D)

#define HCI_RESET OP(3, 0x0003)
#define HCI_READ_STORED_LINK_KEY OP(3, 0x000D)
#define HCI_WRITE_STORED_LINK_KEY OP(3, 0x0011)
#define HCI_DELETE_STORED_LINK_KEY OP(3, 0x0012)
#define HCI_READ_LOCAL_NAME OP(3, 0x0014)
#define HCI_WRITE_LINK_SUPERVISION OP(3, 0x0037)
#define HCI_READ_SCAN_ENABLE OP(3, 0x0019)
#define HCI_WRITE_SCAN_ENABLE OP(3, 0x001A)
#define HCI_WRITE_UNIT_CLASS OP(3, 0x0024)

#define HCI_READ_LOCAL_VERSION OP(4, 0x0001)
#define HCI_READ_LOCAL_COMMANDS OP(4, 0x0002)
#define HCI_READ_LOCAL_FEATURES OP(4, 0x0003)
#define HCI_READ_BUFFER_SIZE OP(4, 0x0005)
#define HCI_READ_BD_ADDR OP(4, 0x0009)

static void connection_complete(Bt *bt, const u8 *bdaddr) {
  u8 p[11];
  p[0] = 0; /* success */
  wr16le(p + 1, bt->handle);
  memcpy(p + 3, bdaddr, 6);
  p[9] = 1; /* ACL link */
  p[10] = 0; /* encryption off */
  bt->connected = 1;
  bt->l2c_stage = 0;
  bt->cfg_req_sent = 0;
  bt->l2c_in = BT_L2CAP_DELAY;
  event_push(bt, EVT_CONNECTION_COMPLETE, p, sizeof(p));
}

void bt_hci_command(Bt *bt, const u8 *cmd, u32 len) {
  if (len < 3u)
    return;
  u16 opcode = rd16le(cmd);
  const u8 *par = cmd + 3;
  u32 par_len = cmd[2];
  if (par_len > len - 3u)
    par_len = len - 3u;
  bt->hci_cmds++;
  if (bt->cmd_log_n < sizeof(bt->cmd_log) / sizeof(bt->cmd_log[0]))
    bt->cmd_log[bt->cmd_log_n++] = opcode;

  switch (opcode) {
  case HCI_RESET:
    bt->connected = 0;
    bt->intr_up = 0;
    bt->l2c_stage = bt->l2c_in = 0;
    bt->cfg_req_sent = 0;
    bt->cid_ctrl_host = bt->cid_intr_host = 0;
    cmd_complete_status(bt, opcode);
    return;

  case HCI_READ_BUFFER_SIZE: {
    u8 r[7];
    wr16le(r, 339);  /* ACL payload the dongle accepts */
    r[2] = 64;       /* SCO payload */
    wr16le(r + 3, 10);
    wr16le(r + 5, 0);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_LOCAL_VERSION: {
    u8 r[8];
    r[0] = 2; /* HCI 1.2, which is what the Wii's own dongle reports */
    wr16le(r + 1, 0x0229);
    r[3] = 2; /* LMP 1.2 */
    wr16le(r + 4, 0x000F);
    wr16le(r + 6, 0x0229);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_LOCAL_FEATURES: {
    /* The feature bits the Wii's stack checks: ACL packet types, role switch
     * and sniff mode. */
    static const u8 r[8] = {0xFFu, 0xFFu, 0x8Fu, 0xFEu,
                            0x9Fu, 0xF9u, 0x00u, 0x80u};
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_LOCAL_COMMANDS: {
    u8 r[64];
    memset(r, 0xFFu, sizeof(r));
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_BD_ADDR:
    cmd_complete(bt, opcode, bt->bdaddr, 6);
    return;

  case HCI_READ_LOCAL_NAME: {
    u8 r[248];
    memset(r, 0, sizeof(r));
    memcpy(r, "Nintendo RVL-WBC-01", 19);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_STORED_LINK_KEY: {
    /* This is how a synced remote comes back after a power cycle: the dongle
     * hands the stack the keys it kept, and the stack pages those addresses.
     * Reporting our one remote here is what makes it "already synced", so the
     * game never needs a SYNC button nobody has. */
    u8 keys[22];
    memcpy(keys, bt->wm_bdaddr, 6);
    memcpy(keys + 6, bt->link_key, 16);
    u8 ev[1 + 22];
    ev[0] = 1;
    memcpy(ev + 1, keys, 22);
    event_push(bt, EVT_RETURN_LINK_KEYS, ev, sizeof(ev));
    u8 r[4];
    wr16le(r, 10); /* max keys the dongle can hold */
    wr16le(r + 2, 1);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_WRITE_STORED_LINK_KEY: {
    u8 r[1];
    r[0] = par_len ? par[0] : 1u;
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_DELETE_STORED_LINK_KEY: {
    u8 r[2];
    wr16le(r, 0);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_CREATE_CONNECTION:
    cmd_status(bt, opcode);
    connection_complete(bt, par_len >= 6u ? par : bt->wm_bdaddr);
    return;

  case HCI_ACCEPT_CONNECTION: {
    cmd_status(bt, opcode);
    const u8 *who = par_len >= 6u ? par : bt->wm_bdaddr;
    /* The seventh byte is the role the host wants: 0 means "switch me to
     * master while you set the link up". A controller that honours it reports
     * the switch before the connection completes, and BTM will not bring an
     * ACL up as master without having seen it. */
    if (par_len >= 7u && par[6] == 0u) {
      u8 r[8];
      r[0] = 0;
      memcpy(r + 1, who, 6);
      r[7] = 0; /* now master */
      event_push(bt, EVT_ROLE_CHANGE, r, sizeof(r));
    }
    connection_complete(bt, who);
    return;
  }

  case HCI_DISCONNECT: {
    cmd_status(bt, opcode);
    u8 p[4];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    p[3] = par_len >= 3u ? par[2] : 0x13u;
    bt->connected = 0;
    bt->intr_up = 0;
    bt->l2c_stage = bt->l2c_in = 0;
    bt->cfg_req_sent = 0;
    bt->cid_ctrl_host = bt->cid_intr_host = 0;
    event_push(bt, EVT_DISCONNECTION_COMPLETE, p, sizeof(p));
    return;
  }

  case HCI_REMOTE_NAME_REQUEST: {
    cmd_status(bt, opcode);
    u8 p[255];
    memset(p, 0, sizeof(p));
    p[0] = 0;
    memcpy(p + 1, par_len >= 6u ? par : bt->wm_bdaddr, 6);
    memcpy(p + 7, "Nintendo RVL-CNT-01", 19);
    event_push(bt, EVT_REMOTE_NAME_COMPLETE, p, 255);
    return;
  }

  case HCI_READ_REMOTE_FEATURES: {
    cmd_status(bt, opcode);
    u8 p[11];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    static const u8 feat[8] = {0xBCu, 0x02u, 0x04u, 0x38u,
                               0x08u, 0x00u, 0x00u, 0x00u};
    memcpy(p + 3, feat, 8);
    event_push(bt, EVT_REMOTE_FEATURES, p, sizeof(p));
    return;
  }

  case HCI_READ_REMOTE_VERSION: {
    cmd_status(bt, opcode);
    u8 p[8];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    p[3] = 2;
    wr16le(p + 4, 0x000Fu);
    wr16le(p + 6, 0x0229u);
    event_push(bt, EVT_REMOTE_VERSION, p, sizeof(p));
    return;
  }

  case HCI_AUTH_REQUESTED: {
    cmd_status(bt, opcode);
    u8 p[3];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    event_push(bt, EVT_AUTH_COMPLETE, p, sizeof(p));
    return;
  }

  case HCI_LINK_KEY_REPLY:
  case HCI_LINK_KEY_NEG_REPLY:
    cmd_complete(bt, opcode, par_len >= 6u ? par : bt->wm_bdaddr, 6);
    return;

  case HCI_SET_CONN_ENCRYPTION: {
    cmd_status(bt, opcode);
    u8 p[4];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    p[3] = par_len >= 3u ? par[2] : 1u;
    event_push(bt, EVT_ENCRYPTION_CHANGE, p, sizeof(p));
    return;
  }

  case HCI_SNIFF_MODE:
  case HCI_EXIT_SNIFF_MODE: {
    cmd_status(bt, opcode);
    u8 p[6];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    p[3] = opcode == HCI_SNIFF_MODE ? 2u : 0u;
    wr16le(p + 4, opcode == HCI_SNIFF_MODE ? 0x0190u : 0u);
    event_push(bt, EVT_MODE_CHANGE, p, sizeof(p));
    return;
  }

  case HCI_READ_CLOCK_OFFSET: {
    cmd_status(bt, opcode);
    u8 p[5];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    wr16le(p + 3, 0x1234u);
    event_push(bt, EVT_READ_CLOCK_OFFSET, p, sizeof(p));
    return;
  }

  case HCI_CHANGE_PACKET_TYPE: {
    cmd_status(bt, opcode);
    u8 p[5];
    p[0] = 0;
    wr16le(p + 1, bt->handle);
    wr16le(p + 3, par_len >= 4u ? rd16le(par + 2) : 0xCC18u);
    event_push(bt, EVT_CONN_PACKET_TYPE_CHANGED, p, sizeof(p));
    return;
  }

  case HCI_INQUIRY:
    /* Nothing is discoverable: the remote is presented as already synced, so
     * a scan started by the SYNC button simply finds nobody. */
    cmd_status(bt, opcode);
    {
      u8 p[1] = {0};
      event_push(bt, EVT_INQUIRY_COMPLETE, p, 1);
    }
    return;

  case HCI_INQUIRY_CANCEL:
    cmd_complete_status(bt, opcode);
    return;

  case HCI_WRITE_LINK_POLICY:
  case HCI_WRITE_LINK_SUPERVISION: {
    /* Both answer with the handle they acted on. */
    u8 r[2];
    wr16le(r, bt->handle);
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_READ_SCAN_ENABLE: {
    u8 r[1] = {bt->scan_enable};
    cmd_complete(bt, opcode, r, sizeof(r));
    return;
  }

  case HCI_WRITE_SCAN_ENABLE:
    bt->scan_enable = par_len >= 1u ? par[0] : 0u;
    cmd_complete_status(bt, opcode);
    /* Bit 1 is page scan: the console has started listening for a remote to
     * connect to it. That is the moment the synced remote pages us -- on a
     * real console, the moment the player presses a button. */
    bt->page_in =
        (bt->scan_enable & 0x02u) && !bt->connected ? BT_PAGE_DELAY : 0u;
    return;

  default:
    /* The rest of what the stack sends is configuration -- event masks, scan
     * windows, timeouts, the class of device -- which this model has nothing
     * to store. Acknowledging them is the whole contract. */
    if ((opcode >> 10) == 3u || (opcode >> 10) == 4u) {
      cmd_complete_status(bt, opcode);
    } else {
      /* Link control and link policy commands are answered with a Command
       * Status, not a Command Complete; a stack that gets the wrong one waits
       * for an event that never comes. */
      bt->hci_unknown++;
      cmd_status(bt, opcode);
    }
    return;
  }
}

/* ---- L2CAP --------------------------------------------------------------- */

#define L2CAP_CID_SIGNAL 0x0001u
#define PSM_HID_CONTROL 0x0011u
#define PSM_HID_INTERRUPT 0x0013u

static void acl_send(Bt *bt, u16 cid, const u8 *payload, u32 len) {
  u8 pkt[BT_PKT_MAX];
  if (len + 8u > BT_PKT_MAX)
    return;
  wr16le(pkt, (u16)(bt->handle | 0x2000u)); /* first packet of a message */
  wr16le(pkt + 2, (u16)(len + 4u));
  wr16le(pkt + 4, (u16)len);
  wr16le(pkt + 6, cid);
  memcpy(pkt + 8, payload, len);
  queue_push(&bt->acl_in, pkt, len + 8u);
}

static void sig_send(Bt *bt, u8 code, u8 id, const u8 *data, u32 len) {
  u8 p[64];
  if (len + 4u > sizeof(p))
    return;
  p[0] = code;
  p[1] = id;
  wr16le(p + 2, (u16)len);
  memcpy(p + 4, data, len);
  acl_send(bt, L2CAP_CID_SIGNAL, p, len + 4u);
}

/* The remote has no say in the channel configuration, so its own request is a
 * bare one and its response always accepts. Sending a request of our own is
 * still required: L2CAP configures each direction separately, and the SDK will
 * not treat a channel as open until both halves are done.
 *
 * Exactly one request per channel, though. A second one puts the SDK's channel
 * back into CST_CONFIG and clears both halves of its config_done; it answers
 * that request, so the inbound half comes back, but it never repeats its own
 * request, so the outbound half never does. The channel then sits in
 * configuration for good and every L2CA_DataWrite on it is dropped. */
static void config_request(Bt *bt, u16 dcid) {
  u8 bit = dcid == bt->cid_ctrl_host ? 1u : 2u;
  if (bt->cfg_req_sent & bit)
    return;
  bt->cfg_req_sent |= bit;
  u8 p[4];
  wr16le(p, dcid);
  wr16le(p + 2, 0);
  sig_send(bt, 0x04u, bt->sig_id++, p, sizeof(p));
}

/* The remote opens a HID channel itself. The source id it picks here is the
 * one the SDK will address it by for the life of the channel. */
static void connect_request(Bt *bt, u16 psm) {
  u16 scid = bt->next_cid++;
  if (psm == PSM_HID_CONTROL) {
    bt->cid_ctrl_dev = scid;
    bt->l2c_stage = 1;
  } else {
    bt->cid_intr_dev = scid;
    bt->l2c_stage = 3;
  }
  u8 p[4];
  wr16le(p, psm);
  wr16le(p + 2, scid);
  sig_send(bt, 0x02u, bt->sig_id++, p, sizeof(p));
}

static void sig_handle(Bt *bt, const u8 *p, u32 len) {
  while (len >= 4u) {
    u8 code = p[0];
    u8 id = p[1];
    u16 plen = rd16le(p + 2);
    if (plen + 4u > len)
      break;
    const u8 *d = p + 4;
    switch (code) {
    case 0x02u: { /* connection request */
      u16 psm = rd16le(d);
      u16 scid = rd16le(d + 2);
      u16 dcid = (u16)(bt->next_cid++ | 0x0000u);
      if (psm == PSM_HID_CONTROL) {
        bt->cid_ctrl_host = scid;
        bt->cid_ctrl_dev = dcid;
      } else if (psm == PSM_HID_INTERRUPT) {
        bt->cid_intr_host = scid;
        bt->cid_intr_dev = dcid;
      }
      u8 r[8];
      wr16le(r, dcid);
      wr16le(r + 2, scid);
      wr16le(r + 4, psm == PSM_HID_CONTROL || psm == PSM_HID_INTERRUPT ? 0u
                                                                       : 2u);
      wr16le(r + 6, 0);
      sig_send(bt, 0x03u, id, r, sizeof(r));
      break;
    }
    case 0x03u: { /* connection response, to a request of our own */
      u16 dcid = rd16le(d);
      u16 scid = rd16le(d + 2);
      u16 result = rd16le(d + 4);
      if (result == 1u) /* pending: the real answer is still to come */
        break;
      if (result != 0u) {
        bt->l2c_stage = 0;
        break;
      }
      if (scid == bt->cid_ctrl_dev)
        bt->cid_ctrl_host = dcid;
      else if (scid == bt->cid_intr_dev)
        bt->cid_intr_host = dcid;
      config_request(bt, dcid);
      break;
    }

    case 0x04u: { /* configuration request */
      u16 dcid = rd16le(d);
      u8 r[6];
      wr16le(r, dcid == bt->cid_ctrl_dev ? bt->cid_ctrl_host
                                         : bt->cid_intr_host);
      wr16le(r + 2, 0);
      wr16le(r + 4, 0); /* success, with no options of our own */
      sig_send(bt, 0x05u, id, r, sizeof(r));
      if (dcid == bt->cid_intr_dev)
        bt->intr_up = 1;
      config_request(bt, dcid == bt->cid_ctrl_dev ? bt->cid_ctrl_host
                                                  : bt->cid_intr_host);
      break;
    }
    case 0x05u: { /* configuration response */
      u16 scid = rd16le(d);
      u16 result = rd16le(d + 4);
      /* Control first, then interrupt: a remote does not ask for the second
       * channel until the first is configured. */
      if (result == 0u && scid == bt->cid_ctrl_dev && bt->l2c_stage == 1u)
        connect_request(bt, PSM_HID_INTERRUPT);
      else if (result == 0u && scid == bt->cid_intr_dev && bt->l2c_stage == 3u)
        bt->l2c_stage = 4;
      if (scid == bt->cid_intr_host || scid == bt->cid_intr_dev)
        bt->intr_up = 1;
      break;
    }
    case 0x06u: { /* disconnection request */
      u8 r[4];
      memcpy(r, d, 4);
      sig_send(bt, 0x07u, id, r, sizeof(r));
      bt->intr_up = 0;
      break;
    }
    case 0x08u: /* echo request */
      sig_send(bt, 0x09u, id, d, plen);
      break;
    case 0x0Au: { /* information request */
      u8 r[4];
      wr16le(r, rd16le(d));
      wr16le(r + 2, 1); /* not supported */
      sig_send(bt, 0x0Bu, id, r, sizeof(r));
      break;
    }
    default:
      break;
    }
    p += 4u + plen;
    len -= 4u + plen;
  }
}

/* ---- HID ----------------------------------------------------------------- */

static u32 report_payload_len(u8 mode) {
  switch (mode) {
  case 0x30u: return 2;
  case 0x31u: return 5;
  case 0x32u: return 10;
  case 0x33u: return 17;
  case 0x34u: return 21;
  case 0x35u: return 21;
  case 0x36u: return 21;
  case 0x37u: return 21;
  case 0x3Du: return 21;
  default: return 2;
  }
}

/* One IR "object" as the remote's extended (12-byte) format encodes it: two
 * objects of x, y and size, with the high bits of each coordinate folded into
 * the size byte. Only the first object is lit; the rest read as absent. */
static void fill_ir(Bt *bt, u8 *p, u32 n, int extended) {
  /* An absent object reads as x = y = 0x3FF, so all-ones is "nothing seen". */
  memset(p, 0xFFu, n);
  if (bt->input.ir_x < 0 || !bt->ir_on)
    return;
  u16 x = (u16)bt->input.ir_x;
  u16 y = (u16)bt->input.ir_y;
  if (extended) {
    /* Four objects of x, y, size, each carrying its own high bits. */
    p[0] = (u8)x;
    p[1] = (u8)y;
    p[2] = (u8)(((y >> 8) << 6) | ((x >> 8) << 4) | 3u);
  } else {
    /* Two objects per five bytes, sharing one byte of high bits. */
    p[0] = (u8)x;
    p[1] = (u8)y;
    p[2] = (u8)(0xF0u | ((y >> 8) << 2) | (x >> 8));
  }
}

static void fill_ext(Bt *bt, u8 *p, u32 n) {
  memset(p, 0, n);
  if (!bt->ext_enabled || n < 6u)
    return;
  /* Nunchuk, in the clear: stick, accelerometers, then the button byte with
   * the two accelerometer low bits above it. C and Z are active low. */
  p[0] = bt->input.stick_x;
  p[1] = bt->input.stick_y;
  p[2] = bt->input.nunchuk_acc[0];
  p[3] = bt->input.nunchuk_acc[1];
  p[4] = bt->input.nunchuk_acc[2];
  p[5] = (u8)(~bt->input.nunchuk & 0x03u);
}

static void send_input_report(Bt *bt) {
  if (!bt->intr_up || !bt->cid_intr_host)
    return;
  u8 mode = bt->report_mode;
  u32 n = report_payload_len(mode);
  u8 p[32];
  memset(p, 0, sizeof(p));
  p[0] = 0xA1u; /* HID DATA, input */
  p[1] = mode;
  wr16be(p + 2, bt->input.buttons);
  u8 *body = p + 4;
  switch (mode) {
  case 0x31u:
    memcpy(body, bt->input.acc, 3);
    break;
  case 0x32u:
    fill_ext(bt, body, 8);
    break;
  case 0x33u:
    memcpy(body, bt->input.acc, 3);
    fill_ir(bt, body + 3, 12, 1);
    break;
  case 0x34u:
    fill_ext(bt, body, 19);
    break;
  case 0x35u:
    memcpy(body, bt->input.acc, 3);
    fill_ext(bt, body + 3, 16);
    break;
  case 0x36u:
    fill_ir(bt, body, 10, 0);
    fill_ext(bt, body + 10, 9);
    break;
  case 0x37u:
    memcpy(body, bt->input.acc, 3);
    fill_ir(bt, body + 3, 10, 0);
    fill_ext(bt, body + 13, 6);
    break;
  default:
    break;
  }
  acl_send(bt, bt->cid_intr_host, p, 2u + n);
  bt->reports++;
}

static void send_ack(Bt *bt, u8 report_id, u8 error) {
  if (!bt->cid_intr_host)
    return;
  u8 p[6];
  p[0] = 0xA1u;
  p[1] = 0x22u;
  wr16be(p + 2, bt->input.buttons);
  p[4] = report_id;
  p[5] = error;
  acl_send(bt, bt->cid_intr_host, p, 6);
}

static void send_status(Bt *bt) {
  if (!bt->cid_intr_host)
    return;
  u8 p[8];
  p[0] = 0xA1u;
  p[1] = 0x20u;
  wr16be(p + 2, bt->input.buttons);
  p[4] = (u8)((bt->leds & 0xF0u) | (bt->ext_enabled ? 0x02u : 0u) |
              (bt->speaker_on ? 0x04u : 0u) | (bt->ir_on ? 0x08u : 0u));
  p[5] = 0;
  p[6] = 0;
  p[7] = 0xC0u; /* battery: full */
  acl_send(bt, bt->cid_intr_host, p, 8);
}

/* The remote's address space. A read or write names it with one control byte:
 * bit 2 picks the peripheral registers over the EEPROM, and only the extension
 * port's page at 0xA400xx is ever touched there. */
static u8 mem_read_byte(Bt *bt, int regs, u32 off) {
  if (!regs)
    return off < BT_EEPROM_SIZE ? bt->eeprom[off] : 0u;
  if ((off & 0xFFFF00u) == 0xA40000u)
    return bt->ext_regs[off & 0xFFu];
  return 0;
}

static void mem_write_byte(Bt *bt, int regs, u32 off, u8 value) {
  if (!regs) {
    if (off < BT_EEPROM_SIZE)
      bt->eeprom[off] = value;
    return;
  }
  if ((off & 0xFFFF00u) == 0xA40000u)
    bt->ext_regs[off & 0xFFu] = value;
}

static void send_read_data(Bt *bt, int regs, u32 off, u16 size) {
  if (!bt->cid_intr_host)
    return;
  /* The remote answers a read in 16-byte instalments, each stamped with how
   * much of it is valid and where it came from. */
  while (size) {
    u16 chunk = size > 16u ? 16u : size;
    u8 p[23];
    p[0] = 0xA1u;
    p[1] = 0x21u;
    wr16be(p + 2, bt->input.buttons);
    p[4] = (u8)((chunk - 1u) << 4);
    wr16be(p + 5, (u16)off);
    memset(p + 7, 0, 16);
    for (u16 i = 0; i < chunk; i++)
      p[7 + i] = mem_read_byte(bt, regs, off + i);
    acl_send(bt, bt->cid_intr_host, p, sizeof(p));
    off += chunk;
    size -= chunk;
  }
}

static void hid_output(Bt *bt, const u8 *p, u32 len) {
  if (len < 2u)
    return;
  u8 id = p[1];
  const u8 *d = p + 2;
  u32 dlen = len - 2u;
  bt->last_out_report = id;
  if (id >= 0x10u && id <= 0x1Fu)
    bt->out_seen |= 1u << (id - 0x10u);
  if (dlen)
    bt->rumble = d[0] & 1u;
  switch (id) {
  case 0x11u: /* player LEDs */
    if (dlen)
      bt->leds = (u8)(d[0] & 0xF0u);
    send_ack(bt, id, 0);
    return;
  case 0x12u: /* set reporting mode */
    if (dlen >= 2u) {
      bt->continuous = (d[0] & 0x04u) != 0;
      bt->report_mode = d[1];
    }
    send_ack(bt, id, 0);
    send_input_report(bt);
    return;
  case 0x13u: /* IR camera enable */
  case 0x1Au: /* IR camera logic enable */
    if (dlen)
      bt->ir_on = (d[0] & 0x04u) != 0;
    send_ack(bt, id, 0);
    return;
  case 0x14u: /* speaker enable */
  case 0x19u: /* speaker mute */
    if (dlen)
      bt->speaker_on = (d[0] & 0x04u) != 0;
    send_ack(bt, id, 0);
    return;
  case 0x15u: /* status request */
    send_status(bt);
    return;
  case 0x16u: { /* write memory */
    /* Control byte, three-byte offset, length, then sixteen bytes of data. */
    if (dlen < 21u)
      return;
    int regs = (d[0] & 0x04u) != 0;
    u32 off = ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3];
    u8 size = d[4] > 16u ? 16u : d[4];
    for (u8 i = 0; i < size; i++)
      mem_write_byte(bt, regs, off + i, d[5 + i]);
    /* Writing 0x55 to the extension port's init register is how the SDK brings
     * the port up; from then on it reports a Nunchuk, in the clear. */
    if (regs && (off & 0xFFFF00u) == 0xA40000u) {
      if ((off & 0xFFu) == 0xF0u && size && d[5] == 0x55u)
        bt->ext_enabled = 1;
      if (bt->ext_enabled) {
        static const u8 nunchuk_id[6] = {0, 0, 0xA4u, 0x20u, 0, 0};
        memcpy(bt->ext_regs + 0xFAu, nunchuk_id, 6);
        /* Neutral factory calibration: centred stick with the usual travel,
         * and a one-g reading of 0xB3 on each accelerometer axis. */
        static const u8 cal[16] = {0x80u, 0x80u, 0x80u, 0x00u, 0xB3u, 0xB3u,
                                   0xB3u, 0x00u, 0xE0u, 0x20u, 0x80u, 0xE0u,
                                   0x20u, 0x80u, 0x00u, 0x00u};
        memcpy(bt->ext_regs + 0x20u, cal, 16);
        memcpy(bt->ext_regs + 0x30u, cal, 16);
      }
    }
    send_ack(bt, id, 0);
    return;
  }
  case 0x17u: { /* read memory */
    /* Control byte, three-byte offset, two-byte length. */
    if (dlen < 6u)
      return;
    int regs = (d[0] & 0x04u) != 0;
    u32 off = ((u32)d[1] << 16) | ((u32)d[2] << 8) | d[3];
    send_read_data(bt, regs, off, rd16be(d + 4));
    return;
  }
  case 0x10u:
  case 0x18u: /* rumble only, speaker data */
    return;
  default:
    send_ack(bt, id, 0);
    return;
  }
}

void bt_acl_write(Bt *bt, const u8 *pkt, u32 len) {
  bt->acl_in_n++;
  if (len < 8u)
    return;
  u16 l2len = rd16le(pkt + 4);
  u16 cid = rd16le(pkt + 6);
  const u8 *payload = pkt + 8;
  if (l2len > len - 8u)
    l2len = (u16)(len - 8u);
  /* The dongle owes the stack a buffer credit for every packet it takes. */
  {
    u8 p[5];
    p[0] = 1;
    wr16le(p + 1, bt->handle);
    wr16le(p + 3, 1);
    event_push(bt, EVT_NUM_COMPLETED_PACKETS, p, sizeof(p));
  }
  if (cid == L2CAP_CID_SIGNAL) {
    sig_handle(bt, payload, l2len);
    return;
  }
  if (cid == bt->cid_ctrl_dev || cid == bt->cid_intr_dev) {
    if (l2len && (payload[0] & 0xF0u) == 0xA0u)
      hid_output(bt, payload, l2len);
  }
}

/* The remote pages the console: a Connection Request the stack answers with
 * Accept_Connection_Request. The class of device is a Wii Remote's own
 * (0x002504), which is how BTM recognises what connected. */
static void connection_request(Bt *bt) {
  u8 p[10];
  memcpy(p, bt->wm_bdaddr, 6);
  p[6] = 0x04u;
  p[7] = 0x25u;
  p[8] = 0x00u;
  p[9] = 1; /* ACL link */
  event_push(bt, EVT_CONNECTION_REQUEST, p, sizeof(p));
}

void bt_tick(Bt *bt) {
  if (bt->page_in && --bt->page_in == 0u && !bt->connected)
    connection_request(bt);
  if (bt->l2c_in && --bt->l2c_in == 0u && bt->connected && !bt->l2c_stage)
    connect_request(bt, PSM_HID_CONTROL);
  if (bt->connected && bt->intr_up && bt->continuous)
    send_input_report(bt);
}

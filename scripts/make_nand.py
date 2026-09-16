#!/usr/bin/env python3
"""Generate the minimal Wii NAND tree the chassis serves over /dev/fs.

Nothing here comes from a console or a disc: SYSCONF and setting.txt are
synthesised from the formats Dolphin documents (Core/Wii/SysConf.cpp and
Common/SettingsHandler.cpp), so the output is safe to regenerate anywhere
and carries no Nintendo data.

  ./scripts/make_nand.py [--root generated/nand] [--nickname GalaxyVita]
"""

import argparse
import os
import struct
import sys

SYSCONF_SIZE = 0x4000

BIG_ARRAY, SMALL_ARRAY, BYTE, SHORT, LONG, LONGLONG, BYTEBOOL = 1, 2, 3, 4, 5, 6, 7


# The Bluetooth address of the Wii Remote the chassis synthesises, in the
# little-endian order HCI puts on the wire -- the same bytes runtime/bt.c sends,
# or the stack would page an address the controller never answers to. SYSCONF
# keeps addresses the other way round, the way a console prints them, because
# the SDK's stack reverses every address it parses out of an HCI packet; the
# entry below is written back to front for that reason.
WIIMOTE_BDADDR = bytes([0x01, 0x00, 0x00, 0x1D, 0x19, 0x00])


def bt_dinf() -> bytes:
    """BT.DINF, the console's list of synced Wii Remotes.

    One byte of count, then eight registered and eight active entries of a
    six-byte address and a 0x40-byte name -- 0x461 bytes in all, which is the
    size SYSCONF reserves. Registering a remote here is what makes the SDK's
    Bluetooth stack page it at boot instead of sitting in its init sequence
    waiting for a SYNC button press nobody can reach.
    """
    entry = bytearray(0x46)
    entry[0:6] = WIIMOTE_BDADDR[::-1]
    name = b"Nintendo RVL-CNT-01"
    entry[6:6 + len(name)] = name
    out = bytearray(0x461)
    out[0] = 1  # one registered device
    out[1:1 + 0x46] = entry                    # registered[0]
    out[1 + 8 * 0x46:1 + 9 * 0x46] = entry     # active[0]
    return bytes(out)


def sysconf_entries(nickname: str, widescreen: bool, language: int):
    nick = nickname[:10]
    # 2 bytes per UTF-16BE character, NUL terminator, then the length byte.
    nick_bytes = bytearray(22)
    nick_bytes[: len(nick) * 2] = nick.encode("utf-16-be")
    nick_bytes[21] = len(nick)

    ipl_sadr = bytearray(0x1007 + 1)
    ipl_sadr[0] = 0x6C  # USA
    ipl_pc = bytearray(0x49 + 1)
    ipl_pc[1] = 0x04
    ipl_pc[2] = 0x14

    return [
        (BIG_ARRAY, "BT.DINF", bt_dinf()),
        (BIG_ARRAY, "BT.CDIF", bytes(0x204 + 1)),
        (LONG, "BT.SENS", bytes([0, 0, 0, 3])),
        (BYTE, "BT.BAR", bytes([1])),
        (BYTE, "BT.SPKV", bytes([0x58])),
        (BYTE, "BT.MOT", bytes([1])),
        (SMALL_ARRAY, "IPL.NIK", bytes(nick_bytes)),
        (BYTE, "IPL.LNG", bytes([language])),
        (BIG_ARRAY, "IPL.SADR", bytes(ipl_sadr)),
        (SMALL_ARRAY, "IPL.PC", bytes(ipl_pc)),
        (LONG, "IPL.CB", bytes(4)),
        (BYTE, "IPL.AR", bytes([1 if widescreen else 0])),
        (BYTE, "IPL.SSV", bytes([1])),
        (BYTEBOOL, "IPL.CD", bytes([0])),
        (BYTEBOOL, "IPL.CD2", bytes([0])),
        (BYTEBOOL, "IPL.EULA", bytes([1])),
        (BYTE, "IPL.UPT", bytes([2])),
        (BYTE, "IPL.PGS", bytes([0])),
        (BYTE, "IPL.E60", bytes([1])),
        (BYTE, "IPL.DH", bytes([0])),
        (LONG, "IPL.INC", bytes([0, 0, 0, 8])),
        (LONG, "IPL.FRC", bytes([0, 0, 0, 0x28])),
        (SMALL_ARRAY, "IPL.IDL", bytes([0, 1])),
        (LONG, "NET.WCFG", bytes([0, 0, 0, 1])),
        (LONG, "NET.CTPC", bytes(4)),
        (BYTE, "WWW.RST", bytes([0])),
        (BYTEBOOL, "MPLS.MOVIE", bytes([1])),
    ]


def build_sysconf(entries) -> bytes:
    header = bytearray(b"SCv0")
    header += struct.pack(">H", len(entries))
    # Offset table holds one extra past-the-end entry.
    entries_begin = len(header) + 2 * (len(entries) + 1)

    body = bytearray()
    offsets = []
    for etype, name, data in entries:
        offsets.append(entries_begin + len(body))
        body.append((etype << 5) | (len(name) - 1))
        body += name.encode("ascii")
        if etype == BIG_ARRAY:
            body += struct.pack(">H", len(data) - 1)
        elif etype == SMALL_ARRAY:
            body.append(len(data) - 1)
        body += data

    out = bytearray(header)
    for off in offsets:
        out += struct.pack(">H", off)
    out += struct.pack(">H", entries_begin + len(body))
    out += body
    if len(out) > SYSCONF_SIZE:
        raise SystemExit("SYSCONF overflowed 0x4000 bytes")
    out += bytes(SYSCONF_SIZE - len(out))
    out[-4:] = b"SCed"
    return bytes(out)


def build_setting_txt(values) -> bytes:
    """setting.txt is a 256-byte buffer XORed with a rotating key."""
    buf = bytearray(256)
    key = 0x73B5DBFA
    pos = 0

    def write_byte(b):
        nonlocal key, pos
        if pos >= len(buf):
            return
        buf[pos] = b ^ (key & 0xFF)
        pos += 1
        key = ((key >> 31) | (key << 1)) & 0xFFFFFFFF

    def write_line(text):
        nonlocal key, pos
        old_pos, old_key = pos, key
        for ch in text:
            write_byte(ord(ch))
        # Nintendo's decoder stops at a NUL byte; prepend an LF and retry.
        if 0 in buf[old_pos:pos]:
            pos, key = old_pos, old_key
            write_byte(0x0A)
            write_line(text)

    for k, v in values:
        write_line(f"{k}={v}\r\n")
    return bytes(buf)


def decode_setting_txt(data: bytes) -> str:
    key = 0x73B5DBFA
    out = []
    for b in data:
        out.append(b ^ (key & 0xFF))
        key = ((key >> 31) | (key << 1)) & 0xFFFFFFFF
    return bytes(out).decode("latin-1").replace("\r", "")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default="generated/nand")
    ap.add_argument("--nickname", default="GalaxyVita")
    ap.add_argument("--serial", default="123456789")
    ap.add_argument("--language", type=int, default=1, help="IPL.LNG (1 = English)")
    ap.add_argument("--aspect", choices=("4:3", "16:9"), default="16:9",
                    help="IPL.AR; the Vita panel is 16:9")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    sysconf_dir = os.path.join(root, "shared2", "sys")
    setting_dir = os.path.join(root, "title", "00000001", "00000002", "data")
    os.makedirs(sysconf_dir, exist_ok=True)
    os.makedirs(setting_dir, exist_ok=True)

    sysconf = build_sysconf(
        sysconf_entries(args.nickname, args.aspect == "16:9", args.language))
    with open(os.path.join(sysconf_dir, "SYSCONF"), "wb") as f:
        f.write(sysconf)

    setting = build_setting_txt([
        ("AREA", "USA"),
        ("MODEL", "RVL-001(USA)"),
        ("DVD", "0"),
        ("MPCH", "0x7FFE"),
        ("CODE", "LU"),
        ("SERNO", args.serial),
        ("VIDEO", "NTSC"),
        ("GAME", "US"),
    ])
    with open(os.path.join(setting_dir, "setting.txt"), "wb") as f:
        f.write(setting)

    # Titles the SDK expects to be able to stat.
    for d in ("title/00010000/524d4745/data", "title/00010000/524d4745/content",
              "meta", "import", "tmp"):
        os.makedirs(os.path.join(root, d), exist_ok=True)

    print(f"SYSCONF  {len(sysconf)} bytes -> {sysconf_dir}/SYSCONF")
    print(f"setting  {len(setting)} bytes -> {setting_dir}/setting.txt")
    print(decode_setting_txt(setting).strip())
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Lay out a mounted, writable DMG's window without Finder: write the .DS_Store that Finder
would save after you arranged the window by hand. Python's standard library only.

  tools/dmg-window.py /Volumes/Inductive --background .background/background.tiff \
      --window 660x428 --icon-size 128 --text-size 13 \
      --item "Inductive.app=180,200" --item "Applications=480,200"

The window has no toolbar, sidebar or status bar; positions are icon centres in points
from the window's top left. The background is a file on the volume, recorded as an alias
(the reference Finder resolves), so it is found wherever the DMG is mounted.

.DS_Store is a "Bud1" buddy-allocated file holding one B-tree ("DSDB") of records
(filename, 4-letter code, typed value); see Wim Lewis's description of the format.
"""
import argparse
import ctypes
import ctypes.util
import os
import plistlib
import struct
import sys

HFS_EPOCH = 2082844800  # seconds from 1904-01-01 to 1970-01-01


def hfs_time(unix_seconds):
    return int(unix_seconds + HFS_EPOCH) & 0xFFFFFFFF


def volume_created(mount):
    """The volume's creation time (getattrlist ATTR_CMN_CRTIME with ATTR_VOL_INFO)."""
    libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)

    class AttrList(ctypes.Structure):
        _fields_ = [("bitmapcount", ctypes.c_ushort), ("reserved", ctypes.c_uint16),
                    ("commonattr", ctypes.c_uint32), ("volattr", ctypes.c_uint32),
                    ("dirattr", ctypes.c_uint32), ("fileattr", ctypes.c_uint32),
                    ("forkattr", ctypes.c_uint32)]

    request = AttrList(5, 0, 0x00000200, 0x80000000, 0, 0, 0)  # ATTR_CMN_CRTIME, ATTR_VOL_INFO
    buffer = ctypes.create_string_buffer(64)
    if libc.getattrlist(mount.encode(), ctypes.byref(request), buffer, len(buffer), 0) != 0:
        raise OSError(ctypes.get_errno(), "getattrlist failed for " + mount)
    seconds, = struct.unpack_from("=q", buffer.raw, 4)
    return seconds


def alias(mount, volume_name, relative_path):
    """A version 2 Alias record for a file on the volume, as macOS writes one."""
    path = os.path.join(mount, relative_path)
    parent = os.path.dirname(path)
    file_name = os.path.basename(path)
    parent_name = os.path.basename(parent)

    def pascal(text, room):
        data = text.encode("mac_roman")[:room - 1]
        return bytes([len(data)]) + data + b"\0" * (room - 1 - len(data))

    def tag(number, data):
        # Its true length, then the data padded to an even size.
        return struct.pack(">hH", number, len(data)) + data + b"\0" * (len(data) % 2)

    def unicode(text):
        encoded = text.encode("utf-16-be")
        return struct.pack(">H", len(encoded) // 2) + encoded

    file_stat = os.stat(path)
    fixed = struct.pack(
        ">4sHHH28sIHHI64sII4s4shhIH10s",
        b"\0\0\0\0", 0, 2, 0,                         # user type, size (filled below), version 2, kind 0 = file
        pascal(volume_name, 28), hfs_time(volume_created(mount)),
        0x482B, 0,                                    # 'H+' (HFS+), fixed disk: as macOS records a mounted DMG
        os.stat(parent).st_ino, pascal(file_name, 64), file_stat.st_ino,
        hfs_time(file_stat.st_birthtime), b"\0\0\0\0", b"\0\0\0\0",
        -1, -1, 0, 0, b"\0" * 10)
    extras = b"".join([
        tag(0, parent_name.encode("utf-8")),                                     # parent folder name
        tag(1, struct.pack(">I", os.stat(parent).st_ino)),                       # CNIDs up to the root
        tag(2, (volume_name + ":" + relative_path.replace("/", ":")).encode("utf-8")),  # HFS path
        tag(14, unicode(file_name)),
        tag(15, unicode(volume_name)),
        tag(18, ("/" + relative_path).encode("utf-8")),                          # POSIX path on the volume
        # No mount point (tag 19): with another volume of the same name mounted, Finder
        # followed that path to the wrong disk. It finds this one by name, date and IDs.
        struct.pack(">hH", -1, 0)])
    record = bytearray(fixed + extras)
    struct.pack_into(">H", record, 4, len(record))
    return bytes(record)


def ds_store(records):
    """A complete .DS_Store holding the records, all in one B-tree leaf."""
    def encode(name, code, kind, value):
        out = struct.pack(">I", len(name)) + name.encode("utf-16-be") + code + kind
        if kind == b"blob":
            return out + struct.pack(">I", len(value)) + value
        if kind == b"long":
            return out + struct.pack(">I", value)
        if kind == b"type":
            return out + value
        raise ValueError(kind)

    ordered = sorted(records, key=lambda r: (r[0].lower(), r[1]))
    leaf = struct.pack(">II", 0, len(ordered)) + b"".join(encode(*r) for r in ordered)
    if len(leaf) > 4096:
        raise ValueError("records do not fit one B-tree page")

    # Allocator space (offsets relative to file offset 4), 8 KiB: header 0x0 (32),
    # DSDB master 0x20 (32), root block 0x800 (2 KiB), leaf 0x1000 (4 KiB); the rest free.
    space = bytearray(0x2000)
    blocks = [0x800 | 11, 0x20 | 5, 0x1000 | 12]       # block 0 root, 1 master, 2 leaf
    master = struct.pack(">IIIII", 2, 0, len(ordered), 1, 0x1000)
    space[0x20:0x20 + len(master)] = master
    root = struct.pack(">II", len(blocks), 0)
    root += b"".join(struct.pack(">I", b) for b in blocks) + b"\0" * 4 * (256 - len(blocks))
    root += struct.pack(">I", 1) + bytes([4]) + b"DSDB" + struct.pack(">I", 1)
    free = {6: [0x40], 7: [0x80], 8: [0x100], 9: [0x200], 10: [0x400]}
    for size_class in range(32):
        offsets = free.get(size_class, [])
        root += struct.pack(">I", len(offsets)) + b"".join(struct.pack(">I", o) for o in offsets)
    space[0x800:0x800 + len(root)] = root
    space[0x1000:0x1000 + len(leaf)] = leaf
    header = struct.pack(">4sIII16s", b"Bud1", 0x800, 0x800, 0x800, b"\0" * 16)
    space[0:len(header)] = header
    return struct.pack(">I", 1) + bytes(space)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("volume")
    parser.add_argument("--background", required=True)
    parser.add_argument("--window", default="660x428")
    parser.add_argument("--icon-size", type=float, default=128)
    parser.add_argument("--text-size", type=float, default=13)
    parser.add_argument("--item", action="append", default=[])
    parser.add_argument("--volume-name", default="Inductive")
    args = parser.parse_args()

    width, height = (int(v) for v in args.window.split("x"))
    window = {
        "WindowBounds": "{{200, 120}, {%d, %d}}" % (width, height),
        "ShowToolbar": False, "ShowSidebar": False, "ShowStatusBar": False, "ShowPathbar": False,
        "ShowTabView": False, "ContainerShowSidebar": False, "PreviewPaneVisibility": False,
        "SidebarWidth": 0}
    view = {
        "viewOptionsVersion": 1, "arrangeBy": "none", "iconSize": args.icon_size,
        "textSize": args.text_size, "labelOnBottom": True, "showItemInfo": False,
        "showIconPreview": True, "gridSpacing": 100.0, "gridOffsetX": 0.0, "gridOffsetY": 0.0,
        "scrollPositionX": 0.0, "scrollPositionY": 0.0,
        "backgroundType": 2, "backgroundColorRed": 1.0, "backgroundColorGreen": 1.0,
        "backgroundColorBlue": 1.0,
        "backgroundImageAlias": alias(args.volume, args.volume_name, args.background)}
    records = [
        (".", b"bwsp", b"blob", plistlib.dumps(window, fmt=plistlib.FMT_BINARY)),
        (".", b"icvp", b"blob", plistlib.dumps(view, fmt=plistlib.FMT_BINARY)),
        (".", b"icvl", b"type", b"icnv"),
        (".", b"vSrn", b"long", 1)]
    for item in args.item:
        name, position = item.rsplit("=", 1)
        x, y = (int(v) for v in position.split(","))
        if not os.path.lexists(os.path.join(args.volume, name)):
            sys.exit("no %s on %s" % (name, args.volume))
        records.append((name, b"Iloc", b"blob", struct.pack(">IIII", x, y, 0xFFFFFFFF, 0xFFFF0000)))

    with open(os.path.join(args.volume, ".DS_Store"), "wb") as out:
        out.write(ds_store(records))


if __name__ == "__main__":
    main()

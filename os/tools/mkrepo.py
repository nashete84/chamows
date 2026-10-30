#!/usr/bin/env python3
"""Pack the app repository (the "Chamows disk") into one blob.

Usage: mkrepo.py <out.bin> <icons dir> <app dir>:<elf> [...]

Layout:
  header   "CHREPO01", u32 count, u32 reserved            (16 bytes)
  entries  count x 256 bytes (see repo_entry_t in kernel/gui.h)
  data     each app's ELF and its icons (24+32+48 px BGRA), 16-byte aligned
"""
import os
import struct
import sys


def manifest(path):
    m = {}
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            m[k.strip()] = v.strip()
    return m


def field(s, n):
    b = s.encode("utf-8")
    if len(b) >= n:
        sys.exit(f"mkrepo: '{s}' no cabe en {n - 1} bytes")
    return b + bytes(n - len(b))


def main():
    out, icons, apps = sys.argv[1], sys.argv[2], sys.argv[3:]
    head = 16 + 256 * len(apps)
    data = bytearray()
    entries = bytearray()

    def put(blob):
        while (head + len(data)) % 16:
            data.append(0)
        off = head + len(data)
        data.extend(blob)
        return off

    for spec in apps:
        appdir, elf = spec.split(":")
        m = manifest(os.path.join(appdir, "manifest.ini"))
        elf_bytes = open(elf, "rb").read()
        icon = open(os.path.join(icons, m["icon"] + ".rgba"), "rb").read()
        assert len(icon) == 4 * (24 * 24 + 32 * 32 + 48 * 48)
        elf_off = put(elf_bytes)
        icon_off = put(icon)
        e = (field(m["id"], 24) + field(m["name"], 40) + field(m["category"], 24)
             + field(m["desc"], 120) + field(m["version"], 12)
             + struct.pack("<IIII", elf_off, len(elf_bytes), icon_off, (len(elf_bytes) + 1023) // 1024)
             + bytes(20))
        assert len(e) == 256
        entries += e
    blob = b"CHREPO01" + struct.pack("<II", len(apps), 0) + entries + data
    open(out, "wb").write(blob)
    print(f"mkrepo: {len(apps)} apps, {len(blob) // 1024} KB")


if __name__ == "__main__":
    main()

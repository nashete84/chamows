#!/usr/bin/env python3
"""Make an empty pendrive image for testing: MBR + one FAT32 partition.

Usage: mkfat32.py <image> <size in MB>   (minimum 40 MB)
Same layout as a pendrive formatted on Windows or macOS (partition at 1 MB).
"""
import struct
import sys

path, mb = sys.argv[1], int(sys.argv[2])
if mb < 40:
    sys.exit("mkfat32: hace falta al menos 40 MB para FAT32")
total = mb * 2048
start = 2048
part = total - start
spc = 1 if mb <= 260 else 8 if mb <= 8192 else 16 if mb <= 16384 else 32
reserved, nfats = 32, 2
fatsz = -(-(part - reserved) // ((256 * spc + nfats) // 2))
clusters = (part - reserved - nfats * fatsz) // spc

bpb = bytearray(512)
bpb[0:3] = b"\xEB\x58\x90"
bpb[3:11] = b"CHAMOWS "
struct.pack_into("<HBHBHHBHHHII", bpb, 11, 512, spc, reserved, nfats, 0, 0, 0xF8, 0, 63, 255, start, part)
struct.pack_into("<IHHIHH", bpb, 36, fatsz, 0, 0, 2, 1, 6)
bpb[64] = 0x80
bpb[66] = 0x29
struct.pack_into("<I", bpb, 67, 0x20261030)
bpb[71:82] = b"CHAMOWS    "
bpb[82:90] = b"FAT32   "
bpb[510:512] = b"\x55\xAA"

fsinfo = bytearray(512)
struct.pack_into("<I", fsinfo, 0, 0x41615252)
struct.pack_into("<III", fsinfo, 484, 0x61417272, clusters - 1, 3)
struct.pack_into("<I", fsinfo, 508, 0xAA550000)

fat = struct.pack("<III", 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)
label = b"CHAMOWS    " + bytes([0x08]) + bytes(20)

mbr = bytearray(512)
mbr[446:462] = struct.pack("<B3sB3sII", 0x00, b"\xFE\xFF\xFF", 0x0C, b"\xFE\xFF\xFF", start, part)
mbr[510:512] = b"\x55\xAA"

with open(path, "wb") as f:
    f.truncate(total * 512)
    def put(lba, data):
        f.seek(lba * 512)
        f.write(data)
    put(0, mbr)
    put(start, bpb)
    put(start + 1, fsinfo)
    put(start + 6, bpb)
    put(start + 7, fsinfo)
    for i in range(nfats):
        put(start + reserved + i * fatsz, fat)
    put(start + reserved + nfats * fatsz, label)  # root folder = cluster 2
print(f"mkfat32: {path}, {mb} MB, {clusters} clústeres de {spc * 512} bytes")

#!/usr/bin/env python3
import struct
import sys

BLOCK_SIZE = 1024

SINGLE_FILE_BYTES = 20 * 1024
DOUBLE_FILE_BYTES = 300 * 1024

TRIPLE_L1_BLOCK = 8188
TRIPLE_L2_BLOCK = 8189
TRIPLE_L3_BLOCK = 8190
TRIPLE_DATA_BLOCK = 8191
TRIPLE_PATTERN = b"ext2 deep indirect block OK\x00"


def write_pattern_file(path, size):
    with open(path, "wb") as f:
        f.write(bytes(i % 256 for i in range(size)))


def write_block(f, block_num, data):
    buf = bytearray(BLOCK_SIZE)
    buf[: len(data)] = data
    f.seek(block_num * BLOCK_SIZE)
    f.write(buf)


def genfiles(rootfs_dir):
    write_pattern_file(f"{rootfs_dir}/singleindirect.txt", SINGLE_FILE_BYTES)
    write_pattern_file(f"{rootfs_dir}/doubleindirect.txt", DOUBLE_FILE_BYTES)


def plant(disk_path):
    with open(disk_path, "r+b") as f:
        write_block(f, TRIPLE_L1_BLOCK, struct.pack("<I", TRIPLE_L2_BLOCK))
        write_block(f, TRIPLE_L2_BLOCK, struct.pack("<I", TRIPLE_L3_BLOCK))
        write_block(f, TRIPLE_L3_BLOCK, struct.pack("<I", TRIPLE_DATA_BLOCK))
        write_block(f, TRIPLE_DATA_BLOCK, TRIPLE_PATTERN)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: ext2_testgen.py genfiles <rootfs_dir> | plant <disk_img>\n")
        return 1

    mode, arg = argv[1], argv[2]

    if mode == "genfiles":
        genfiles(arg)
    elif mode == "plant":
        plant(arg)
    else:
        sys.stderr.write(f"unknown mode: {mode}\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

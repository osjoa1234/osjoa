#!/usr/bin/env python3
import sys

SINGLE_FILE_BYTES = 20 * 1024
DOUBLE_FILE_BYTES = 300 * 1024


def write_pattern_file(path, size):
    with open(path, "wb") as f:
        f.write(bytes(i % 256 for i in range(size)))


def genfiles(rootfs_dir):
    write_pattern_file(f"{rootfs_dir}/singleindirect.txt", SINGLE_FILE_BYTES)
    write_pattern_file(f"{rootfs_dir}/doubleindirect.txt", DOUBLE_FILE_BYTES)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write("usage: ext2_testgen.py genfiles <rootfs_dir>\n")
        return 1

    mode, arg = argv[1], argv[2]

    if mode == "genfiles":
        genfiles(arg)
    else:
        sys.stderr.write(f"unknown mode: {mode}\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

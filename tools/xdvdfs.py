"""Minimal XDVDFS (Xbox/Xbox 360 game disc filesystem) reader.

Usage:
  python xdvdfs.py <iso> list
  python xdvdfs.py <iso> extract <path-in-image> <out-file>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: plain XISO, XGD1, XGD2, XGD3
PARTITION_OFFSETS = [0, 0x18300000, 0xFD90000, 0x2080000]


def find_partition(f):
    for base in PARTITION_OFFSETS:
        f.seek(base + 32 * SECTOR)
        if f.read(20) == MAGIC:
            return base
    raise ValueError("No XDVDFS volume descriptor found")


def walk(f, base, sector, size, prefix=""):
    """Yields (path, is_dir, start_sector, size) for a directory's binary tree."""
    f.seek(base + sector * SECTOR)
    table = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF:  # padding
            continue
        name = table[off + 14:off + 14 + nlen].decode("latin-1")
        path = f"{prefix}/{name}"
        is_dir = bool(attr & 0x10)
        yield path, is_dir, start, length
        if is_dir and length:
            yield from walk(f, base, start, length, path)
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def open_volume(path):
    f = open(path, "rb")
    base = find_partition(f)
    f.seek(base + 32 * SECTOR + 20)
    root_sector, root_size = struct.unpack("<II", f.read(8))
    return f, base, root_sector, root_size


def main():
    iso, cmd = sys.argv[1], sys.argv[2]
    f, base, rs, rsz = open_volume(iso)
    entries = sorted(walk(f, base, rs, rsz))
    if cmd == "list":
        print(f"partition base 0x{base:X}")
        for p, d, s, l in entries:
            print(f"{'<DIR>' if d else l:>12}  {p}")
    elif cmd == "extract":
        want = sys.argv[3].lower().lstrip("/")
        for p, d, s, l in entries:
            if p.lower().lstrip("/") == want and not d:
                os.makedirs(os.path.dirname(os.path.abspath(sys.argv[4])), exist_ok=True)
                f.seek(base + s * SECTOR)
                with open(sys.argv[4], "wb") as out:
                    remaining = l
                    while remaining:
                        chunk = f.read(min(remaining, 1 << 20))
                        out.write(chunk)
                        remaining -= len(chunk)
                print(f"extracted {p} ({l} bytes)")
                return
        sys.exit(f"not found: {want}")


if __name__ == "__main__":
    main()

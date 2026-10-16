#!/usr/bin/env python3
"""Build/deploy-time server<->driver ABI check.

Reads the au_abi_info record (server.h) directly from the ELF files of
<tree>/server and every <tree>/runtime/**/*.dll and fails (exit 1) unless
every driver carries a record byte-identical to the server's. Uses only
readelf, so it never loads or runs the binaries.

Usage: python3 tools/abi_check.py [tree]      (default: current directory)
"""
import os
import re
import struct
import subprocess
import sys

FIELDS = ("magic version ptr_size maxfield sz_map sz_item sz_character sz_effect "
          "off_effect_field_cnt off_effect_m").split()


def readelf(*args):
    return subprocess.run(["readelf", "-W", *args], capture_output=True, text=True, check=True).stdout


def abi_record(path):
    sym = None
    for line in readelf("-s", path).splitlines():
        p = line.split()
        if len(p) >= 8 and p[-1] == "au_abi_info" and p[6] != "UND":
            sym = (int(p[1], 16), int(p[2], 0))
            break
    if not sym:
        return None
    addr, size = sym
    for line in readelf("-S", path).splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if not m or m.group(2) == "NOBITS":
            continue
        start, off, length = int(m.group(3), 16), int(m.group(4), 16), int(m.group(5), 16)
        if start <= addr < start + length:
            with open(path, "rb") as f:
                f.seek(off + addr - start)
                return f.read(size)
    return None


def decode(raw):
    if raw is None or len(raw) < 4 * len(FIELDS):
        return None
    return dict(zip(FIELDS, struct.unpack("<%dI" % len(FIELDS), raw[:4 * len(FIELDS)])))


def main():
    tree = sys.argv[1] if len(sys.argv) > 1 else "."
    server = os.path.join(tree, "server")
    ref_raw = abi_record(server)
    ref = decode(ref_raw)
    if not ref or ref["magic"] != 0x41554142:
        print("FAIL server: no au_abi_info record (%s)" % server)
        return 1
    print("server: " + " ".join("%s=%d" % (k, v) for k, v in ref.items() if k != "magic"))
    bad = total = 0
    for root, _, names in sorted(os.walk(os.path.join(tree, "runtime"))):
        for n in sorted(names):
            if not n.endswith(".dll"):
                continue
            total += 1
            p = os.path.join(root, n)
            raw = abi_record(p)
            if raw == ref_raw:
                continue
            bad += 1
            d = decode(raw)
            if not d:
                print("FAIL %s: no au_abi_info (built before the ABI check)" % os.path.relpath(p, tree))
            else:
                diff = ", ".join("%s %d!=%d" % (k, d[k], ref[k]) for k in FIELDS if d[k] != ref[k])
                print("FAIL %s: %s" % (os.path.relpath(p, tree), diff))
    print("%s: %d/%d drivers match the server ABI" % ("PASS" if not bad else "FAIL", total - bad, total))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

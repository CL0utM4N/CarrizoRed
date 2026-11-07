#!/usr/bin/env python3
"""
Decode an NVRAM dump (OpenCore / UEFI Shell `dmpstore -s nvramNNN.bin`) on any OS.

Prints UnifiedRed's variables (CR-*, and KaberiRed's old KR-*) and the panic report Apple keeps in AAPL,PanicInfo*: the kernel stores it
7-bit packed and split over several variables. The panic log holds only about 10 KB and drops the middle of a long
report, so UnifiedRed puts its most important lines first.

Usage:
    python scripts/dmpstore.py nvram157.bin [--all]     --all lists every variable
"""

import argparse
import struct
import sys
import uuid

APPLE_BOOT_GUID = "7C436110-AB2A-4BBB-A880-FE41995C9F82"


def parse(data):
    """dmpstore -s format: name size, data size, name (UTF-16), GUID, attributes, data, CRC32."""
    off, out = 0, []
    while off + 8 <= len(data):
        name_size, data_size = struct.unpack_from("<II", data, off)
        if name_size == 0 or name_size > 4096 or data_size > 1 << 20:
            break
        name = data[off + 8:off + 8 + name_size].decode("utf-16-le").rstrip("\0")
        guid = str(uuid.UUID(bytes_le=data[off + 8 + name_size:off + 24 + name_size])).upper()
        attributes = struct.unpack_from("<I", data, off + 24 + name_size)[0]
        value = data[off + 28 + name_size:off + 28 + name_size + data_size]
        out.append((guid, name, attributes, value))
        off += 28 + name_size + data_size + 4
    return out, off


def unpack7(packed):
    """The kernel's panic-log packing: 8 characters of 7 bits in every 7 bytes."""
    out, acc, bits = bytearray(), 0, 0
    for byte in packed:
        acc |= byte << bits
        bits += 8
        while bits >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            bits -= 7
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump")
    ap.add_argument("--all", action="store_true", help="list every variable")
    args = ap.parse_args()

    variables, parsed = parse(open(args.dump, "rb").read())
    print(f"{len(variables)} variables, parsed {parsed} bytes")
    for guid, name, attributes, value in variables:
        if args.all or guid == APPLE_BOOT_GUID and (name.startswith(("CR-", "KR-")) or "anic" in name or name == "boot-args"):
            shown = value.decode("ascii", "replace").rstrip("\0") if name == "boot-args" else f"{len(value)} bytes"
            print(f"{guid[:8]} {name:28} {shown}")

    chunks = sorted((name, value) for guid, name, attributes, value in variables if name.startswith("AAPL,PanicInfo"))
    if chunks:
        raw = b"".join(value for name, value in chunks)
        text = unpack7(raw).split(b"\0")[0].decode("ascii", "replace")
        print(f"===== AAPL,PanicInfo ({len(chunks)} chunks, {len(raw)} bytes) =====")
        print(text)
    else:
        print("no AAPL,PanicInfo: the last boot didn't panic (or NVRAM was cleared)")

    for guid, name, attributes, value in variables:
        if name.startswith(("CR-", "KR-")):
            print(f"===== {name} =====")
            print(value.decode("ascii", "replace"))


if __name__ == "__main__":
    sys.exit(main())

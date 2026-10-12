#!/usr/bin/env python3
"""
Extract files from an Apple Kernel Debug Kit (or any Apple .pkg) without a Mac.

Usage:
    python scripts/extract_kdk.py <Kernel_Debug_Kit.dmg | KernelDebugKit.pkg | Payload> -o <outdir> \
        [-m AMD9500Controller.kext -m AMDRadeonX4000 ...] [--list]

    -m/--match  extract only paths containing this text (case-insensitive, repeatable). Default: everything.
    --list      print matching paths instead of extracting.
    --exact     -m is a whole path (e.g. System/Library/Kernels/kernel), not a substring.

A .dmg or .pkg is unpacked with 7-Zip (7z must be installed; it is found on PATH or in Program Files) to get the
KDK.pkg Payload. The Payload is a pbzx stream (xz-compressed chunks) wrapping a cpio archive, both handled here with
the Python standard library and streamed, so the multi-GB contents are never held in memory.

Get KDKs from https://developer.apple.com/download/all/ or the Dortania mirror:
    gh release download <build> -R dortania/KdkSupportPkg -p '*.dmg'
"""

import argparse
import lzma
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path, PurePosixPath


def find_7z():
    for cand in (shutil.which("7z"), shutil.which("7za"),
                 r"C:\Program Files\7-Zip\7z.exe", r"C:\Program Files (x86)\7-Zip\7z.exe"):
        if cand and Path(cand).is_file():
            return cand
    sys.exit("7-Zip not found; install it or pass the extracted Payload file directly")


def run_7z(sevenzip, archive, member, outdir):
    """Extract one member (flat) from an archive with 7-Zip; return the extracted path."""
    subprocess.run([sevenzip, "e", "-y", f"-o{outdir}", str(archive), member],
                   check=True, stdout=subprocess.DEVNULL)
    path = Path(outdir) / PurePosixPath(member.replace("\\", "/")).name
    if not path.is_file():
        sys.exit(f"7-Zip did not produce {member} from {archive}")
    return path


def list_7z(sevenzip, archive):
    out = subprocess.run([sevenzip, "l", "-slt", str(archive)], check=True, capture_output=True, text=True).stdout
    return [line[7:] for line in out.splitlines() if line.startswith("Path = ")]


def payload_from(path, workdir):
    """Return a path to the KDK Payload, unpacking a .dmg/.pkg with 7-Zip as needed."""
    sevenzip = None
    if path.suffix.lower() == ".dmg":
        sevenzip = find_7z()
        pkgs = [p for p in list_7z(sevenzip, path) if p.lower().endswith(".pkg")]
        if not pkgs:
            sys.exit(f"no .pkg inside {path}")
        print(f"[dmg] extracting {pkgs[0]}")
        path = run_7z(sevenzip, path, pkgs[0], workdir)
    if path.suffix.lower() == ".pkg":
        sevenzip = sevenzip or find_7z()
        members = list_7z(sevenzip, path)
        # Prefer the main KDK component; fall back to the largest-looking Payload.
        payloads = [m for m in members if m.replace("\\", "/").endswith("/Payload") or m == "Payload"]
        payloads.sort(key=lambda m: (not m.replace("\\", "/").startswith("KDK.pkg/"), m))
        if not payloads:
            sys.exit(f"no Payload inside {path}")
        print(f"[pkg] extracting {payloads[0]}")
        path = run_7z(sevenzip, path, payloads[0], workdir)
    return path


def pbzx_chunks(fh):
    """Yield decompressed byte chunks from a pbzx stream (or raw bytes if it is not pbzx)."""
    magic = fh.read(4)
    if magic != b"pbzx":
        yield magic
        while chunk := fh.read(1 << 20):
            yield chunk
        return
    (chunk_size,) = struct.unpack(">Q", fh.read(8))
    while True:
        hdr = fh.read(16)
        if not hdr:
            return
        if len(hdr) != 16:
            raise ValueError("truncated pbzx chunk header")
        _flags, length = struct.unpack(">QQ", hdr)
        data = fh.read(length)
        if len(data) != length:
            raise ValueError("truncated pbzx chunk")
        if length == chunk_size or not data.startswith(b"\xfd7zXZ\x00"):
            yield data  # stored uncompressed
        else:
            yield lzma.decompress(data, format=lzma.FORMAT_XZ)


class Stream:
    """Minimal buffered reader over an iterator of byte chunks."""

    def __init__(self, chunks):
        self.chunks = chunks
        self.buf = bytearray()

    def read(self, n):
        while len(self.buf) < n:
            nxt = next(self.chunks, None)
            if nxt is None:
                break
            self.buf += nxt
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    def copy_to(self, n, out):
        """Copy n bytes to file object out (or discard if None) without buffering it all."""
        while n:
            if not self.buf:
                nxt = next(self.chunks, None)
                if nxt is None:
                    raise ValueError("cpio data truncated")
                self.buf += nxt
            take = min(n, len(self.buf))
            if out is not None:
                out.write(self.buf[:take])
            del self.buf[:take]
            n -= take


def cpio_entries(stream):
    """Yield (name, mode, size, mtime) for each cpio entry; caller must consume `size` data bytes."""
    while True:
        magic = stream.read(6)
        if not magic:
            return
        if magic == b"070707":  # odc (what Apple's pkgs use)
            h = stream.read(70)
            mode = int(h[12:18], 8)
            mtime = int(h[42:53], 8)
            namesize = int(h[53:59], 8)
            size = int(h[59:70], 8)
            name = stream.read(namesize)[:-1].decode("utf-8", "surrogateescape")
            pad = 0
        elif magic in (b"070701", b"070702"):  # newc
            h = stream.read(104)
            f = [int(h[i:i + 8], 16) for i in range(0, 104, 8)]
            mode, mtime, size, namesize = f[1], f[5], f[6], f[11]
            name = stream.read(namesize)[:-1].decode("utf-8", "surrogateescape")
            stream.read((-(110 + namesize)) % 4)
            pad = (-size) % 4
        else:
            raise ValueError(f"unknown cpio header {magic!r}")
        if name == "TRAILER!!!":
            return
        yield name, mode, size, mtime
        if pad:
            stream.read(pad)


def safe_rel(name):
    parts = [p for p in PurePosixPath(name).parts if p not in ("", ".", "/")]
    if any(p == ".." for p in parts):
        return None
    return Path(*parts) if parts else None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("-m", "--match", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--exact", action="store_true", help="-m must equal the whole path (after ./), not a substring")
    args = ap.parse_args()

    needles = [m.lower() for m in args.match]
    if args.exact:
        wanted = lambda n: n.lower().lstrip("./") in [x.lstrip("./") for x in needles]
    else:
        wanted = (lambda n: any(x in n.lower() for x in needles)) if needles else (lambda n: True)

    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=args.out) as work:
        payload = payload_from(args.input, work)
        count = total = 0
        with open(payload, "rb") as fh:
            stream = Stream(pbzx_chunks(fh))
            for name, mode, size, mtime in cpio_entries(stream):
                rel = safe_rel(name)
                if rel is None or not wanted(name):
                    stream.copy_to(size, None)
                    continue
                count += 1
                total += size
                if args.list:
                    print(f"{size:12d}  {name}")
                    stream.copy_to(size, None)
                    continue
                dest = args.out / rel
                kind = stat.S_IFMT(mode)
                if kind == stat.S_IFDIR:
                    dest.mkdir(parents=True, exist_ok=True)
                    stream.copy_to(size, None)
                elif kind == stat.S_IFLNK:
                    target = stream.read(size).decode("utf-8", "surrogateescape")
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    # Windows symlinks need privileges; record the target in a text file instead.
                    dest.with_name(dest.name + ".symlink").write_text(target)
                elif kind == stat.S_IFREG:
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    with open(dest, "wb") as out:
                        stream.copy_to(size, out)
                    os.utime(dest, (mtime, mtime))
                else:
                    stream.copy_to(size, None)
        verb = "matched" if args.list else "extracted"
        print(f"{verb} {count} entries, {total / 1048576:.1f} MB")


if __name__ == "__main__":
    main()

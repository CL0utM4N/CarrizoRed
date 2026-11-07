#!/usr/bin/env python3
"""
Extract files from Apple's full macOS installer (InstallAssistant.pkg) over HTTP, without downloading it.

There is no Monterey Kernel Debug Kit on Dortania's mirror (it starts at Ventura), but Monterey still ships every kext
binary on disk, and the installer in Apple's public software update catalog contains the whole system. This reads
only the byte ranges it needs: typically a few hundred MB for a set of kexts, out of a 12 GB installer.

The layers, each addressed with HTTP range requests:
    InstallAssistant.pkg (XAR, SharedSupport.dmg stored uncompressed)
    -> SharedSupport.dmg (UDIF, raw blocks) -> HFS+ volume
    -> com_apple_MobileAsset_MacSoftwareUpdate/<hash>.zip (ZIP64)
    -> AssetData/payloadv2/payload.NNN chunks (pbzx: independent 8 MB xz blocks)
    -> Apple Archive (YAA1) entries: path, type, data.
Each chunk is a byte-sorted run of paths (files grouped into chunks by size), so a path is found by binary search
over the chunk's blocks. AssetData/payloadv2/fixup.manifest lists every path, and is used to check that nothing
expected was missed.

Usage:
    python scripts/fetch_macos.py --build 21H1320 -o ../KaberiRed-RE/Monterey-12.7.6-21H1320 \
        -p System/Library/Extensions/AMD -p System/Library/Extensions/AppleGraphicsControl.kext
    python scripts/fetch_macos.py --build 21H1320 --list -p System/Library/Extensions/AMD
    python scripts/fetch_macos.py --catalog          list the installers in Apple's catalog

    -p/--prefix   extract every file whose path starts with this (repeatable; no leading slash)
    --list        only list the matching paths from fixup.manifest
    --cache DIR   where downloaded blocks are kept between runs (default: <outdir>/.cache)

Symlinks are written as `<name>.symlink` text files (Windows symlinks need admin rights). Downloaded data is Apple's
copyright: keep it outside the repository.
"""

import argparse
import hashlib
import lzma
import os
import plistlib
import re
import struct
import sys
import urllib.request
import zlib
from pathlib import Path

CATALOG = "https://swscan.apple.com/content/catalogs/others/index-12-1.sucatalog"

fetched = 0


# --- HTTP ---------------------------------------------------------------------------------------------------------

class Remote:
    def __init__(self, url):
        self.url = url

    def read(self, off, n):
        global fetched
        for attempt in range(6):
            req = urllib.request.Request(self.url, headers={"Range": f"bytes={off}-{off + n - 1}"})
            try:
                with urllib.request.urlopen(req, timeout=120) as resp:
                    # A plain 200 would be the whole 12 GB file: never read more than asked for.
                    if resp.status != 206:
                        raise IOError(f"HTTP {resp.status} for a range request")
                    data = resp.read(n)
                if len(data) == n:
                    fetched += n
                    return data
            except OSError as e:
                print(f"  retry {attempt + 1} at {off:#x}: {e}", file=sys.stderr)
        raise IOError(f"range {off:#x}+{n:#x} failed")


def find_installer(build=None, version=None):
    cat = plistlib.loads(urllib.request.urlopen(CATALOG, timeout=60).read())
    found = []
    for pid, product in cat["Products"].items():
        pkgs = [p for p in product.get("Packages", []) if p["URL"].endswith("InstallAssistant.pkg")]
        dists = product.get("Distributions", {})
        dist = dists.get("English") or next(iter(dists.values()), None)
        if not pkgs or not dist:
            continue
        text = urllib.request.urlopen(dist, timeout=60).read().decode("utf-8", "replace")
        ver = re.search(r"<key>VERSION</key>\s*<string>([^<]+)", text)
        bld = re.search(r"<key>BUILD</key>\s*<string>([^<]+)", text)
        found.append((ver and ver.group(1), bld and bld.group(1), pkgs[0]["Size"], pkgs[0]["URL"]))
    if build or version:
        found = [f for f in found if (build and f[1] == build) or (version and f[0] == version)]
    return found


# --- Containers ---------------------------------------------------------------------------------------------------

class Volume:
    """The HFS+ partition of SharedSupport.dmg inside the pkg."""

    def __init__(self, remote):
        self.remote = remote
        head = remote.read(0, 65536)
        _, hsize, _, tclen, _, _ = struct.unpack_from(">IHHQQI", head, 0)
        toc = zlib.decompress(head[hsize:hsize + tclen]).decode()
        m = re.search(r"<data><length>(\d+)</length><encoding style=\"application/octet-stream\"></encoding>"
                      r"<offset>(\d+)</offset>(?:(?!<file).)*?<name>SharedSupport.dmg</name>", toc, re.S)
        if not m:
            raise ValueError("no uncompressed SharedSupport.dmg in this package")
        dmg_size, self.dmg = int(m.group(1)), hsize + tclen + int(m.group(2))
        koly = remote.read(self.dmg + dmg_size - 512, 512)
        if koly[:4] != b"koly":
            raise ValueError("SharedSupport.dmg is not a UDIF image")
        xml_off, xml_len = struct.unpack_from(">QQ", koly, 0xD8)
        plist = plistlib.loads(remote.read(self.dmg + xml_off, xml_len))
        for blkx in plist["resource-fork"]["blkx"]:
            if "Apple_HFS" not in blkx["Name"]:
                continue
            data = blkx["Data"]
            start = struct.unpack_from(">Q", data, 0x08)[0]
            self.start = start * 512
            self.chunks = []
            for i in range(struct.unpack_from(">I", data, 0xC8)[0]):
                kind, _, sector, count, coff, _ = struct.unpack_from(">IIQQQQ", data, 0xCC + 40 * i)
                if kind == 0xFFFFFFFF:
                    continue
                if kind not in (0, 1, 2):
                    raise ValueError(f"compressed DMG chunk type {kind:#x} not supported")
                self.chunks.append((kind, (start + sector) * 512, count * 512, coff))
            return
        raise ValueError("no HFS+ partition in SharedSupport.dmg")

    def read(self, off, n):
        out, pos = bytearray(), self.start + off
        while n:
            for kind, base, size, coff in self.chunks:
                if base <= pos < base + size:
                    take = min(n, base + size - pos)
                    out += self.remote.read(self.dmg + coff + pos - base, take) if kind == 1 else bytes(take)
                    pos, n = pos + take, n - take
                    break
            else:
                raise ValueError(f"volume offset {pos:#x} not mapped")
        return bytes(out)


def hfs_fork(data, off):
    size = struct.unpack_from(">Q", data, off)[0]
    extents = [struct.unpack_from(">II", data, off + 16 + 8 * i) for i in range(8)]
    return size, [e for e in extents if e[1]]


class Hfs:
    def __init__(self, vol):
        self.vol = vol
        header = vol.read(1024, 512)
        if header[:2] not in (b"H+", b"HX"):
            raise ValueError("not an HFS+ volume")
        self.block = struct.unpack_from(">I", header, 40)[0]
        self.catalog = hfs_fork(header, 272)

    def read(self, fork, off, n):
        size, extents = fork
        out, pos = bytearray(), 0
        for start, count in extents:
            length = count * self.block
            if off < pos + length and n:
                take = min(pos + length - off, n)
                out += self.vol.read(start * self.block + off - pos, take)
                off, n = off + take, n - take
            pos += length
        if n:
            raise ValueError("file needs the extents overflow file (not supported)")
        return bytes(out)

    def files(self):
        cat = self.read(self.catalog, 0, self.catalog[0])
        node_size = struct.unpack_from(">H", cat, 32)[0]
        node = struct.unpack_from(">I", cat, 24)[0]  # first leaf node
        folders, files = {}, []
        while node:
            nd = cat[node * node_size:(node + 1) * node_size]
            flink, _, _, _, nrec = struct.unpack_from(">IIbBH", nd, 0)
            for i in range(nrec):
                o = struct.unpack_from(">H", nd, node_size - 2 * (i + 1))[0]
                klen, parent, nlen = struct.unpack_from(">HIH", nd, o)
                name = nd[o + 8:o + 8 + 2 * nlen].decode("utf-16-be", "replace")
                r = o + 2 + klen
                kind = struct.unpack_from(">H", nd, r)[0]
                if kind == 1:
                    folders[struct.unpack_from(">I", nd, r + 8)[0]] = (parent, name)
                elif kind == 2:
                    files.append((parent, name, hfs_fork(nd, r + 88)))
            node = flink

        def path(fid):
            parts = []
            while fid in folders:
                fid, name = folders[fid]
                parts.append(name)
            return "/".join(reversed(parts))
        return [(path(p) + "/" + n, f) for p, n, f in files]


class Zip:
    def __init__(self, hfs, fork):
        self.hfs, self.fork = hfs, fork
        tail = self.read(fork[0] - 65536, 65536)
        e = tail.rfind(b"PK\x06\x06")
        if e < 0:
            raise ValueError("no ZIP64 end record")
        cd_size, cd_off = struct.unpack_from("<QQ", tail, e + 40)
        cd = self.read(cd_off, cd_size)
        self.entries, o = {}, 0
        while cd[o:o + 4] == b"PK\x01\x02":
            method = struct.unpack_from("<H", cd, o + 10)[0]
            csize, usize = struct.unpack_from("<II", cd, o + 20)
            nlen, xlen, clen = struct.unpack_from("<HHH", cd, o + 28)
            loff = struct.unpack_from("<I", cd, o + 42)[0]
            name = cd[o + 46:o + 46 + nlen].decode()
            extra, xo = cd[o + 46 + nlen:o + 46 + nlen + xlen], 0
            while xo + 4 <= len(extra):
                tag, size = struct.unpack_from("<HH", extra, xo)
                if tag == 1:
                    values = list(struct.unpack_from(f"<{size // 8}Q", extra, xo + 4))
                    if usize == 0xFFFFFFFF:
                        usize = values.pop(0)
                    if csize == 0xFFFFFFFF:
                        csize = values.pop(0)
                    if loff == 0xFFFFFFFF:
                        loff = values.pop(0)
                xo += 4 + size
            self.entries[name] = (method, csize, usize, loff)
            o += 46 + nlen + xlen + clen

    def read(self, off, n):
        return self.hfs.read(self.fork, off, n)

    def data_offset(self, name):
        loff = self.entries[name][3]
        nlen, xlen = struct.unpack_from("<HH", self.read(loff, 30), 26)
        return loff + 30 + nlen + xlen

    def stream(self, name, piece=4 << 20):
        """Yield the (decompressed) contents of an entry piece by piece."""
        method, csize = self.entries[name][:2]
        pos = self.data_offset(name)
        end = pos + csize
        inflate = zlib.decompressobj(-15) if method == 8 else None
        while pos < end:
            raw = self.read(pos, min(piece, end - pos))
            pos += len(raw)
            yield inflate.decompress(raw) if inflate else raw


def pbzx_decode(pieces):
    """Decompress a whole pbzx stream given as an iterator of byte pieces."""
    buf, out = bytearray(), bytearray()
    it = iter(pieces)

    def need(n):
        while len(buf) < n:
            buf.extend(next(it))
    need(12)
    del buf[:12]
    while True:
        try:
            need(16)
        except StopIteration:
            return bytes(out)
        usize, csize = struct.unpack_from(">QQ", buf, 0)
        need(16 + csize)
        block = bytes(buf[16:16 + csize])
        del buf[:16 + csize]
        out += lzma.decompress(block) if csize != usize else block


# --- Apple Archive ------------------------------------------------------------------------------------------------

def yaa_header(d, o):
    """One YAA1 header at d[o:]: (header size, fields, [(blob key, size)]), or None if it doesn't parse."""
    if d[o:o + 4] != b"YAA1" or o + 6 > len(d):
        return None
    hsize = struct.unpack_from("<H", d, o + 4)[0]
    p, end = o + 6, o + hsize
    if hsize < 6 or end > len(d):
        return None
    fields, blobs = {}, []
    while p < end:
        key, t = d[p:p + 3].decode("latin1"), chr(d[p + 3])
        p += 4
        if t in "1248":
            fields[key] = int.from_bytes(d[p:p + int(t)], "little")
            p += int(t)
        elif t == "P":
            n = struct.unpack_from("<H", d, p)[0]
            fields[key] = d[p + 2:p + 2 + n].decode("utf-8", "replace")
            p += 2 + n
        elif t == "T":
            p += 12
        elif t == "S":
            p += 8
        elif t in "ABC":
            n = {"A": 2, "B": 4, "C": 8}[t]
            blobs.append((key, int.from_bytes(d[p:p + n], "little")))
            p += n
        elif t == "*":
            fields[key] = True
        else:
            return None
    if p != end or "PAT" not in fields or fields.get("TYP") not in (ord("D"), ord("F"), ord("L"), ord("B"),
                                                                    ord("C"), ord("P"), ord("S")):
        return None
    return hsize, fields, blobs


class Chunk:
    """One stored payload.NNN entry: random access to its pbzx blocks, cached on disk."""

    def __init__(self, zip_, name, cache):
        self.zip, self.name, self.cache = zip_, name, cache
        self.base = zip_.data_offset(name)
        self.size = zip_.entries[name][1]
        self.index = cache / (name.split("/")[-1] + ".blocks")
        # (offset of compressed data in the entry, compressed size, uncompressed size) for each block, and the
        # uncompressed position where each block starts. Walked lazily: every block header is 16 bytes in front of
        # its data, so finding block k takes k small reads.
        self.blocks, self.starts, self.complete = [], [], False
        if self.index.exists():
            for line in self.index.read_text().split("\n"):
                if line:
                    self._add(tuple(map(int, line.split())))
            self.complete = True
        self.memo = {}

    def _add(self, block):
        self.starts.append(self.starts[-1] + self.blocks[-1][2] if self.blocks else 0)
        self.blocks.append(block)

    def _walk(self, upto=None):
        o = self.blocks[-1][0] + self.blocks[-1][1] if self.blocks else 12
        while not self.complete and (upto is None or len(self.blocks) <= upto):
            if o >= self.size:
                self.complete = True
                self.index.write_text("\n".join("%d %d %d" % b for b in self.blocks))
                break
            usize, csize = struct.unpack(">QQ", self.zip.read(self.base + o, 16))
            self._add((o + 16, csize, usize))
            o += 16 + csize

    @property
    def count(self):
        self._walk()
        return len(self.blocks)

    @property
    def length(self):
        self._walk()
        return self.starts[-1] + self.blocks[-1][2]

    def block(self, k):
        if k in self.memo:
            return self.memo[k]
        self._walk(k)
        off, csize, usize = self.blocks[k]
        path = self.cache / ("%s.%d" % (self.name.split("/")[-1], k))
        raw = path.read_bytes() if path.exists() else None
        if raw is None or len(raw) != csize:
            raw = self.zip.read(self.base + off, csize)
            path.write_bytes(raw)
        data = lzma.decompress(raw) if csize != usize else raw
        if len(self.memo) > 8:
            self.memo.clear()
        self.memo[k] = data
        return data

    def read(self, pos, n):
        """Bytes at an uncompressed position of the chunk."""
        out = bytearray()
        while n:
            while not self.complete and self.starts[-1:] and pos >= self.starts[-1] + self.blocks[-1][2]:
                self._walk(len(self.blocks))
            if not self.blocks:
                self._walk(0)
            k = max(i for i, s in enumerate(self.starts) if s <= pos)
            if pos >= self.starts[k] + self.blocks[k][2]:
                break  # past the end
            data = self.block(k)
            take = min(n, len(data) - (pos - self.starts[k]))
            out += data[pos - self.starts[k]:pos - self.starts[k] + take]
            pos, n = pos + take, n - take
        return bytes(out)

    def header_at(self, pos):
        head = self.read(pos, 6)
        if head[:4] != b"YAA1":
            return None
        hsize = struct.unpack_from("<H", head, 4)[0]
        return yaa_header(self.read(pos, hsize), 0)

    def first_header(self, k, max_blocks=16):
        """Position and path of the first entry starting at or after block k (checked: the next one parses too)."""
        for j in range(k, k + max_blocks):
            self._walk(j)
            if j >= len(self.blocks):
                break
            data = self.block(j)
            o = data.find(b"YAA1")
            while o >= 0:
                pos = self.starts[j] + o
                h = self.header_at(pos)
                if h and pos == 0:
                    return pos, h[1]["PAT"]  # a chunk starts with an entry: no need to check further
                if h:
                    nxt = pos + h[0] + sum(s for _, s in h[2])
                    if self.header_at(nxt) or not self.read(nxt, 1):
                        return pos, h[1]["PAT"]
                o = data.find(b"YAA1", o + 1)
        return None

    def entries_in_first_block(self):
        size, n, pos = len(self.block(0)), 0, 0
        while pos < size and n < 64:
            h = self.header_at(pos)
            if not h:
                break
            n += 1
            pos += h[0] + sum(s for _, s in h[2])
        return n

    def entries_from(self, pos):
        """Yield (position, fields, blobs, data position) of consecutive entries."""
        while pos < self.length:
            h = self.header_at(pos)
            if not h:
                raise ValueError(f"{self.name}: no entry at {pos:#x}")
            hsize, fields, blobs = h
            yield pos, fields, blobs, pos + hsize
            pos += hsize + sum(s for _, s in blobs)


def key(path):
    return path.encode("utf-8")


# --- Extraction ---------------------------------------------------------------------------------------------------

def extract(zip_, prefixes, outdir, cache, expected):
    chunks = sorted(n for n in zip_.entries if re.fullmatch(r"AssetData/payloadv2/payload\.\d{3}", n))
    print(f"{len(chunks)} payload chunks")
    firsts = {}
    for name in chunks:
        if zip_.entries[name][0] != 0:
            continue  # deflated in the ZIP (large media files): no random access; skipped
        c = Chunk(zip_, name, cache)
        f = c.first_header(0)
        firsts[name] = (c, f[1] if f else None)
    found = {}
    for prefix in prefixes:
        for i, name in enumerate(chunks):
            if name not in firsts or firsts[name][1] is None:
                continue
            c, first = firsts[name]
            if key(prefix) < key(first) and not key(first).startswith(key(prefix)):
                continue
            # The next chunk continues this run (starts later): this chunk ends before it.
            nxt = next((firsts[n][1] for n in chunks[i + 1:] if n in firsts), None)
            if nxt and key(nxt) > key(first) and key(nxt) <= key(prefix):
                continue
            # A chunk of big files (few entries in its first block) is cheapest to walk entry by entry from the
            # start: one block per header, data skipped. Otherwise binary search: the last block whose first entry
            # sorts before the prefix.
            lo, hi, start = 0, c.count - 1, 0
            if c.entries_in_first_block() < 8:
                hi = -1
            while lo <= hi:
                mid = (lo + hi) // 2
                f = c.first_header(mid)
                if f and key(f[1]) < key(prefix):
                    start, lo = f[0], mid + 1
                else:
                    hi = mid - 1
            last = None
            for pos, fields, blobs, data_pos in c.entries_from(start):
                path = fields["PAT"]
                if last is not None and key(path) < key(last):
                    break  # a new run starts inside this chunk
                last = path
                if key(path) > key(prefix) and not path.startswith(prefix):
                    break
                if not path.startswith(prefix) or path in found:
                    continue
                found[path] = name
                write_entry(c, fields, blobs, data_pos, outdir)
            print(f"  {name}: {prefix}: {sum(1 for p in found if p.startswith(prefix))} so far "
                  f"({fetched / 1e6:.0f} MB fetched)")
    # Files with an HLC (hard-link cluster) field share their content with another path and may be stored under
    # that one only: identical version.plist / CodeResources files across kexts, for example.
    missing = [p for p, linked in expected if p not in found and not linked]
    linked = [p for p, linked in expected if p not in found and linked]
    print(f"{len(found)} entries extracted; not found: {len(missing)} files, plus {len(linked)} hard-linked copies "
          "stored under another path")
    for p in missing[:40]:
        print("  missing:", p)


def write_entry(chunk, fields, blobs, data_pos, outdir):
    target = outdir / fields["PAT"]
    kind = chr(fields["TYP"])
    if kind == "D":
        target.mkdir(parents=True, exist_ok=True)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    if kind == "L":
        (target.parent / (target.name + ".symlink")).write_text(fields.get("LNK", ""))
        return
    pos = data_pos
    for blob, size in blobs:
        if blob == "DAT":
            target.write_bytes(chunk.read(pos, size))
            return
        pos += size
    target.write_bytes(b"")


def manifest_paths(zip_, prefixes, cache):
    path = cache / "fixup.manifest"
    if not path.exists():
        path.write_bytes(pbzx_decode(zip_.stream("AssetData/payloadv2/fixup.manifest")))
    d = path.read_bytes()
    o, out = 0, []
    while o < len(d):
        h = yaa_header(d, o)
        if not h:
            raise ValueError(f"fixup.manifest: bad entry at {o:#x}")
        hsize, fields, blobs = h
        if any(fields["PAT"].startswith(p) for p in prefixes) and chr(fields["TYP"]) == "F":
            out.append((fields["PAT"], "HLC" in fields))
        o += hsize + sum(s for _, s in blobs)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build")
    ap.add_argument("--version")
    ap.add_argument("--url", help="InstallAssistant.pkg URL (instead of --build/--version)")
    ap.add_argument("--catalog", action="store_true", help="list the installers in Apple's catalog")
    ap.add_argument("-o", "--outdir", type=Path)
    ap.add_argument("-p", "--prefix", action="append", default=[])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--cache", type=Path)
    args = ap.parse_args()

    if args.catalog:
        for version, build, size, url in sorted(find_installer(), key=lambda f: f[0] or ""):
            print(f"{version:10} {build:10} {size / 1e9:5.1f} GB  {url}")
        return 0
    url = args.url
    if not url:
        hits = find_installer(args.build, args.version)
        if len(hits) != 1:
            sys.exit(f"{len(hits)} installers match; use --catalog to list them")
        url = hits[0][3]
        print(f"macOS {hits[0][0]} ({hits[0][1]}): {url}")
    if not args.outdir or not args.prefix:
        sys.exit("need -o and at least one -p")
    cache = args.cache or args.outdir / ".cache" / hashlib.sha1(url.encode()).hexdigest()[:12]
    cache.mkdir(parents=True, exist_ok=True)

    hfs = Hfs(Volume(Remote(url)))
    update = next(f for p, f in hfs.files() if "MacSoftwareUpdate" in p and p.endswith(".zip"))
    zip_ = Zip(hfs, update)
    prefixes = [p.lstrip("/") for p in args.prefix]
    expected = manifest_paths(zip_, prefixes, cache)
    if args.list:
        print("\n".join(p for p, _ in expected))
        return 0
    extract(zip_, sorted(prefixes, key=key), args.outdir, cache, expected)
    print(f"{fetched / 1e6:.0f} MB fetched in total")
    return 0


if __name__ == "__main__":
    sys.exit(main())

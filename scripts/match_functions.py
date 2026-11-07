#!/usr/bin/env python3
"""
Name functions in a stripped Apple binary from a symbolised build of the same code (`pip install capstone`).

Monterey's HWLibs (AMD's CAIL) are stripped; Sequoia's aren't. CAIL functions are full of register offsets, so each
function is fingerprinted by the set of immediate constants it uses (>= 0x100), and every reference function is
matched to the stripped function with the most similar set (Jaccard index). Function starts in the stripped binary
are found by their `push rbp; mov rbp, rsp` prologue.

    python scripts/match_functions.py <stripped> <reference> <symbol regex> [--min 0.6]

    --installs <stripped addr> <reference symbol>
        pair the function pointers an init function stores (lea reg, [rip+X]; mov [base+off], reg) in the two
        builds, in order: e.g. Cail_Spectre_InitFunctionPointer, whose table names every function it installs.

Prints `reference symbol  imms  best-match (score)  runner-up (score)`. A score near 1 with a clear gap to the runner-up
is reliable; functions with fewer than 3 distinctive constants can't be matched this way.
"""

import argparse
import bisect
import collections
import importlib.util
import re
import sys
from pathlib import Path

import capstone

spec = importlib.util.spec_from_file_location("macho", Path(__file__).with_name("machotool.py"))
macho = importlib.util.module_from_spec(spec)
spec.loader.exec_module(macho)

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def text(m):
    return next(s for s in m.sects if s[1] == "__text")


def instructions(m):
    t = text(m)
    code, off, out = m.read(t[2], t[3]), 0, []
    while off < len(code):
        for i in MD.disasm(code[off:], t[2] + off):
            out.append((i.address, i.size, i.mnemonic, i.op_str))
            off = i.address + i.size - t[2]
        off += 1  # past an undecodable byte (data in code)
    return out


def fingerprints(m, starts):
    starts = sorted(starts)
    out = collections.defaultdict(set)
    for a, size, mnemonic, ops in instructions(m):
        if "rip" in ops or mnemonic.startswith("j") or mnemonic == "call":
            continue
        i = bisect.bisect_right(starts, a) - 1
        if i < 0:
            continue
        for v in re.findall(r"\b0x([0-9a-f]+)\b", ops):
            v = int(v, 16)
            if 0x100 <= v < 0xFFFFFFFF and v != 0xFFFF:
                out[starts[i]].add(v)
    return out


def prologues(m):
    t = text(m)
    return [t[2] + x.start() for x in re.finditer(rb"\x55\x48\x89\xe5", m.read(t[2], t[3]))]


def installs(m, addr, limit=0x1200):
    regs, out = {}, []
    for i in MD.disasm(m.read(addr, limit), addr):
        lea = re.match(r"(\w+), \[rip ([+-]) (0x[0-9a-f]+)\]", i.op_str)
        if i.mnemonic == "lea" and lea:
            sign = 1 if lea.group(2) == "+" else -1
            regs[lea.group(1)] = i.address + i.size + sign * int(lea.group(3), 16)
        store = re.match(r"qword ptr \[r\w+ \+ (0x[0-9a-f]+)\], (\w+)", i.op_str)
        if i.mnemonic == "mov" and store and store.group(2) in regs:
            out.append((int(store.group(1), 16), regs[store.group(2)]))
        if i.mnemonic == "ret":
            break
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stripped")
    ap.add_argument("reference")
    ap.add_argument("regex", nargs="?", default="")
    ap.add_argument("--min", type=float, default=0.0)
    ap.add_argument("--installs", nargs=2, metavar=("ADDR", "SYMBOL"))
    args = ap.parse_args()
    stripped, ref = macho.MachO(args.stripped), macho.MachO(args.reference)

    if args.installs:
        a, b = installs(stripped, int(args.installs[0], 16)), installs(ref, ref.syms[args.installs[1]])
        if len(a) != len(b):
            print(f"warning: {len(a)} stores in the stripped function, {len(b)} in the reference")
        for (off, target), (roff, rtarget) in zip(a, b):
            print(f"+{off:<6x} {target:8x}  {ref.symat(rtarget)} (reference +{roff:x})")
        return 0

    t = text(ref)
    rstarts = [a for a in ref.addr2sym if t[2] <= a < t[2] + t[3]]
    want = [a for a in rstarts if re.search(args.regex, ref.addr2sym[a])]
    rf = fingerprints(ref, rstarts)
    sf = fingerprints(stripped, prologues(stripped))
    for a in sorted(want, key=lambda a: ref.addr2sym[a]):
        imms = rf[a]
        if len(imms) < 3:
            continue
        scored = sorted(((len(imms & s) / len(imms | s), b) for b, s in sf.items() if s), reverse=True)[:2]
        if not scored or scored[0][0] < args.min:
            continue
        print(f"{ref.addr2sym[a][:56]:56} {len(imms):3}  " + "  ".join(f"{b:x} ({j:.2f})" for j, b in scored))
    return 0


if __name__ == "__main__":
    sys.exit(main())

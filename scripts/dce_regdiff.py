#!/usr/bin/env python3
"""
Diff two Linux amdgpu DCE register header sets (e.g. DCE 11.0 vs DCE 11.2).

Usage:
    python3 scripts/dce_regdiff.py <linux>/drivers/gpu/drm/amd/include/asic_reg/dce \
        [--a dce_11_0] [--b dce_11_2] [--csv out.csv] [--out report.txt]

Reads <x>_d.h (register offsets) and <x>_sh_mask.h (bit fields) for both sets and reports:
  - registers present in both with different offsets
  - registers present in only one set
  - B-only/moved direct (mm) registers whose offset is used by a *different* A register (writes that would hit
    the wrong register), separated from pure renames (same offset, instance prefix added or renumbered).
    Indirect (ix) registers are skipped: each block has its own index space, so equal ix offsets mean nothing.
  - registers at the same offset whose bit fields differ
Instanced registers (CRTC0_.., DCP3_.., DIG8_..) are folded into one row ("DCP{n}_GRPH_CONTROL").
"""

import argparse
import csv
import re
import sys
from collections import defaultdict
from pathlib import Path

DEFINE_RE = re.compile(r"^#define\s+(mm|ix)(\w+)\s+(0x[0-9A-Fa-f]+|\d+)")
FIELD_RE = re.compile(r"^#define\s+(\w+?)__(\w+)_MASK\s+(0x[0-9A-Fa-f]+|\d+)")
INSTANCE_RE = re.compile(r"^([A-Z]+)(\d+)_(.+)$")


def parse_regs(path):
    regs = {}
    for line in path.read_text().splitlines():
        m = DEFINE_RE.match(line)
        if m:
            regs[m.group(1) + m.group(2)] = int(m.group(3), 0)
    return regs


def parse_fields(path):
    fields = defaultdict(dict)
    for line in path.read_text().splitlines():
        m = FIELD_RE.match(line)
        if m:
            fields[m.group(1)][m.group(2)] = int(m.group(3), 0)
    return fields


def fold(name):
    """mmDCP3_GRPH_CONTROL -> ('mmDCP{n}_GRPH_CONTROL', 3); mmGRPH_CONTROL -> ('mmGRPH_CONTROL', None)."""
    prefix, rest = name[:2], name[2:]
    m = INSTANCE_RE.match(rest)
    if m:
        return f"{prefix}{m.group(1)}{{n}}_{m.group(3)}", int(m.group(2))
    return name, None


def tail(name):
    """Register name after its last instance token: mmCOL_MAN0_GAMMA_CORR_CNTLA -> GAMMA_CORR_CNTLA,
    mmDC_PERFMON12_PERFMON_CNTL -> PERFMON_CNTL. Used to spot pure renames."""
    m = re.match(r"^.*[A-Z]\d+_(.+)$", name[2:])
    return m.group(1) if m else name[2:]


def block_of(name):
    rest = name[2:]
    m = INSTANCE_RE.match(rest)
    if m:
        return m.group(1)
    return rest.split("_", 1)[0]


def group_names(names):
    """Fold instanced names. Returns {folded: sorted instance list (or [None])}."""
    groups = defaultdict(list)
    for n in names:
        f, i = fold(n)
        groups[f].append(i)
    return {k: sorted(v, key=lambda x: -1 if x is None else x) for k, v in groups.items()}


def fmt_inst(insts):
    nums = [i for i in insts if i is not None]
    if not nums:
        return ""
    return ",".join(str(i) for i in nums)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", type=Path)
    ap.add_argument("--a", default="dce_11_0", help="baseline (our hardware)")
    ap.add_argument("--b", default="dce_11_2", help="what Apple's driver expects")
    ap.add_argument("--csv", type=Path, help="write every difference as CSV")
    ap.add_argument("--out", type=Path, help="write the text report to a file")
    args = ap.parse_args()

    ra = parse_regs(args.dir / f"{args.a}_d.h")
    rb = parse_regs(args.dir / f"{args.b}_d.h")
    fa = parse_fields(args.dir / f"{args.a}_sh_mask.h")
    fb = parse_fields(args.dir / f"{args.b}_sh_mask.h")

    common = ra.keys() & rb.keys()
    moved = sorted(n for n in common if ra[n] != rb[n])
    only_a = sorted(ra.keys() - rb.keys())
    only_b = sorted(rb.keys() - ra.keys())

    # Offset -> names, per space (mm direct / ix indirect).
    by_off_a = defaultdict(set)
    for n, o in ra.items():
        by_off_a[(n[:2], o)].add(n)

    collisions = []  # B register whose offset belongs to an unrelated A register
    renamed = []     # same register at the same offset, only the name changed (instance prefix added/renumbered)
    for n in only_b + moved:
        if not n.startswith("mm"):
            continue
        hits = by_off_a.get((n[:2], rb[n]), set()) - {n}
        if not hits:
            continue
        if any(tail(h) == tail(n) for h in hits):
            renamed.append((n, rb[n], sorted(hits)))
        else:
            collisions.append((n, rb[n], sorted(hits)))

    # Field differences for registers with the same name and offset in both.
    field_diffs = []
    for n in sorted(common - set(moved)):
        reg = n[2:]
        a, b = fa.get(reg, {}), fb.get(reg, {})
        if not a and not b:
            continue
        if a != b:
            field_diffs.append((n, a, b))

    rows = []
    for n in moved:
        rows.append(("moved", n, hex(ra[n]), hex(rb[n]), ""))
    for n in only_a:
        rows.append((f"only_{args.a}", n, hex(ra[n]), "", ""))
    for n in only_b:
        rows.append((f"only_{args.b}", n, "", hex(rb[n]), ""))
    for n, off, hits in collisions:
        rows.append(("collision", n, "", hex(off), " ".join(hits)))
    for n, off, hits in renamed:
        rows.append(("renamed", n, "", hex(off), " ".join(hits)))
    for n, a, b in field_diffs:
        detail = []
        for f in sorted(a.keys() | b.keys()):
            if a.get(f) != b.get(f):
                detail.append(f"{f}:{hex(a[f]) if f in a else '-'}->{hex(b[f]) if f in b else '-'}")
        rows.append(("fields", n, "", "", " ".join(detail)))

    if args.csv:
        with args.csv.open("w", newline="") as fh:
            w = csv.writer(fh)
            w.writerow(["kind", "register", args.a, args.b, "detail"])
            w.writerows(rows)

    out = []
    p = out.append
    p(f"Registers: {args.a} {len(ra)}, {args.b} {len(rb)}, common {len(common)}")
    p(f"Moved: {len(moved)}  only {args.a}: {len(only_a)}  only {args.b}: {len(only_b)}  "
      f"collisions: {len(collisions)}  renamed: {len(renamed)}  field diffs: {len(field_diffs)}")

    def block_counts(names):
        c = defaultdict(int)
        for f in group_names(names):
            c[block_of(f)] += 1
        return sorted(c.items(), key=lambda kv: (-kv[1], kv[0]))

    for title, names in (("MOVED", moved), (f"ONLY {args.a}", only_a), (f"ONLY {args.b}", only_b)):
        p(f"\n## {title} (folded register count by block)")
        p("  " + "  ".join(f"{b}:{c}" for b, c in block_counts(names)))

    p("\n## MOVED (folded)")
    for f, insts in sorted(group_names(moved).items()):
        # Show instance-0 or generic offsets as the representative pair.
        rep = f.replace("{n}", str(insts[0])) if insts[0] is not None else f
        p(f"  {f:60s} {hex(ra[rep]):>8} -> {hex(rb[rep]):<8} inst[{fmt_inst(insts)}]")

    for title, names, regs in ((f"ONLY {args.a}", only_a, ra), (f"ONLY {args.b}", only_b, rb)):
        p(f"\n## {title} (folded)")
        for f, insts in sorted(group_names(names).items()):
            rep = f.replace("{n}", str(insts[0])) if insts[0] is not None else f
            p(f"  {f:60s} {hex(regs[rep]):>8} inst[{fmt_inst(insts)}]")

    p("\n## COLLISIONS (b register -> unrelated a register at same offset)")
    for n, off, hits in collisions:
        p(f"  {n:55s} {hex(off):>8} -> {', '.join(hits)}")

    p("\n## FIELD DIFFS")
    for n, a, b in field_diffs:
        p(f"  {n}")
        for f in sorted(a.keys() | b.keys()):
            if a.get(f) != b.get(f):
                av = hex(a[f]) if f in a else "-"
                bv = hex(b[f]) if f in b else "-"
                p(f"      {f:50s} {av:>12} -> {bv}")

    text = "\n".join(out)
    if args.out:
        args.out.write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    sys.exit(main())

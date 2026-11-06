#!/usr/bin/env python3
"""What a macOS release still has for GCN 2 (CIK) video: UVD 4.2 decode and VCE 2.0 encode.

Checks Apple's AMDRadeonX4000 (and optionally a HWLibs plug-in) for:
  - the CIK/SI UVD and VCE classes (hardware engines, rings, channels, and the shared multimedia layer's
    per-generation classes AMDUVDCIK / AMDCIKVCE);
  - the VCE firmware images embedded in the accelerator (located through their "[ATI LIB=VCEFW,x.y.z]" tags);
  - the UVD/VCE register offsets the UVD rings and channels write, which tell VM-mode (UVD 5+) code from
    UVD 3/4 code: on UVD 4.2, 0x3DA1 is UVD_RBC_IB_BASE and 0x3DA3 UVD_RBC_RB_BASE, on UVD 6 the same
    offsets are UVD_LMI_RBC_IB_VMID / UVD_LMI_RBC_RB_VMID;
  - in HWLibs: CAIL's UVD/VCE functions, and which name a GCN 2 chip (Bonaire, Hawaii, Spectre, Kalindi,
    Godavari);
  - optionally, how much of a Linux firmware image (linux-firmware amdgpu/*_vce.bin, *_uvd.bin) is present.

Usage (Windows or anywhere, Python 3 with capstone, as macho.py needs):
  python scripts/video_gcn2_check.py --accel <AMDRadeonX4000> [--hwlibs <AMDRadeonX4000HWLibs>] [--fw kaveri_vce.bin ...]

The binaries come from a KDK (scripts/extract_kdk.py); symbols are needed for the class and function checks,
so use the KDK's copies, not a release build's (those are stripped).
"""
import argparse, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from macho import MachO  # noqa: E402

CLASSES = [
    # hardware layer
    'AMDCIUVDHWEngine', 'AMDSIUVDHWEngine', 'AMDSIUVDRing', 'AMDSIUVDChannel',
    'AMDCIVCEHWEngine', 'AMDCIVCERing', 'AMDCIVCEChannel', 'AMDCIVCELLQRing', 'AMDCIVCELLQChannel',
    'AMDVIUVDHWEngine', 'AMDUVD6v3HWEngine', 'AMDVIVCEHWEngine', 'AMDVCE3v4HWEngine',
    # shared multimedia layer (firmware, messages, commands) per generation
    'AMDUVDTrinity', 'AMDUVDCIK', 'AMDUVDVI', 'AMDUVD6v3', 'AMDSIVCE', 'AMDCIKVCE', 'AMDVIVCE', 'AMDVCE3v4',
    # chip hardware classes
    'AMDCIHardware', 'AMDBonaireHardware', 'AMDHawaiiHardware', 'AMDVIHardware',
]
# UVD register offsets, dword index, with their UVD 4.2 / UVD 6.0 names (Linux asic_reg/uvd)
UVD_REGS = {
    0x3C66: (None, 'LMI_RBC_IB_64BIT_BAR_HIGH'), 0x3C67: (None, 'LMI_RBC_IB_64BIT_BAR_LOW'),
    0x3C68: (None, 'LMI_RBC_RB_64BIT_BAR_HIGH'), 0x3C69: (None, 'LMI_RBC_RB_64BIT_BAR_LOW'),
    0x3D26: ('LMI_EXT40_ADDR', 'LMI_EXT40_ADDR'), 0x3DA1: ('RBC_IB_BASE', 'LMI_RBC_IB_VMID'),
    0x3DA2: ('RBC_IB_SIZE', 'RBC_IB_SIZE'), 0x3DA3: ('RBC_RB_BASE', 'LMI_RBC_RB_VMID'),
}
CHIPS = re.compile(r'bonaire|hawaii|spectre|kalindi|godavari|kaveri|kabini|mullins|_ci_|cik', re.I)


def class_present(m, cls):
    """By symbol (hardware classes), else by the OSMetaClass name string (the multimedia-layer classes have no
    exported symbols)."""
    full = f'AMDRadeonX4000_{cls}'
    if any(f'{len(full)}{full}' in n for n in m.syms):
        return 'symbols'
    return 'name string' if (full.encode() + b'\0') in m.d else None


def function_names(m, cls):
    out = set()
    full = f'AMDRadeonX4000_{cls}'
    pre = f'__ZN{len(full)}{full}'
    for n in m.syms:
        if n.startswith(pre):
            mm = re.match(r'(\d+)', n[len(pre):])
            if mm:
                k = int(mm.group(1)); out.add(n[len(pre) + len(mm.group(1)):][:k])
    return sorted(out - {'MetaClass', 'gMetaClass', 'superClass', 'metaClass'})


def uvd_register_writes(m):
    """Immediates in the UVD ring/channel functions that are UVD register offsets."""
    res = {}
    for name in m.syms:
        mm = re.match(r'__ZN(\d+)AMDRadeonX4000_', name)
        if not mm:
            continue
        cl = int(mm.group(1)); cls = name[len(mm.group(0)) - 15:][:cl][15:]
        rest = name[4 + len(mm.group(1)) + cl:]
        mk = re.match(r'(\d+)', rest)
        if 'UVD' not in cls or not re.search(r'(Ring|Channel)$', cls) or not mk:
            continue
        meth = rest[len(mk.group(1)):][:int(mk.group(1))]
        if meth not in ('registerLocation', 'initializeIndirectBufferCmd', 'init'):
            continue
        try:
            text = m.disasm(name, 0x800)
        except Exception:
            continue
        regs = {int(v, 16) for v in re.findall(r'0x([0-9a-f]+)', '\n'.join(text)) if int(v, 16) in UVD_REGS}
        if regs:
            res[f'{cls}::{meth}'] = sorted(regs)
    return res


def vce_images(data):
    out = []
    for mt in re.finditer(rb'\[ATI LIB=VCEFW,([0-9.]+)\]', data):
        out.append((mt.start(), mt.group(1).decode()))
    return out


def fw_presence(data, fw):
    d = Path(fw).read_bytes()
    usize, uoff = struct.unpack_from('<II', d, 0x14)
    body = d[uoff:uoff + usize]
    chunks = [body[o:o + 64] for o in range(0, len(body) - 64, 64)]
    chunks = [c for c in chunks if len(set(c)) >= 24]
    hits = sorted(p for p in (data.find(c) for c in chunks) if p >= 0)
    return len(hits), len(chunks), (hits[0], hits[-1]) if hits else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--accel', required=True, help='AMDRadeonX4000 binary (KDK copy, with symbols)')
    ap.add_argument('--hwlibs', help='an AMDRadeonX4xxxHWLibs binary (KDK copy)')
    ap.add_argument('--fw', nargs='*', default=[], help='Linux firmware images to look for')
    a = ap.parse_args()

    m = MachO(a.accel)
    print(f'## {a.accel}  ({len(m.syms)} symbols)')
    if len(m.syms) < 1000:
        print('   few symbols: probably a stripped release build; class checks will be incomplete')
    print('\nClasses:')
    for c in CLASSES:
        how = class_present(m, c)
        print(f'   {"yes" if how else " no"}  {c}' + (f'  ({how})' if how == 'name string' else ''))
    for c in ('AMDCIVCEHWEngine', 'AMDCIVCERing', 'AMDCIVCEChannel'):
        if class_present(m, c) == 'symbols':
            print(f'   {c}: {", ".join(function_names(m, c))}')

    print('\nVCE firmware tags (file offset, version):')
    for off, ver in vce_images(m.d):
        print(f'   {off:#x}  VCEFW {ver}')

    print('\nUVD register offsets written by UVD rings/channels (4.2 name / 6.0 name):')
    for fn, regs in sorted(uvd_register_writes(m).items()):
        print(f'   {fn}: ' + ', '.join(f'{r:#x} ({UVD_REGS[r][0] or "-"} / {UVD_REGS[r][1]})' for r in regs))

    for fw in a.fw:
        n, total, span = fw_presence(m.d, fw)
        print(f'\n{Path(fw).name}: {n}/{total} 64-byte chunks in the accelerator' + (f', {span[0]:#x}-{span[1]:#x}' if span else ''))

    if a.hwlibs:
        h = MachO(a.hwlibs)
        print(f'\n## {a.hwlibs}  ({len(h.syms)} symbols)')
        names = sorted(n for n in h.syms if re.search(r'uvd|vce', n, re.I))
        chip = [n for n in names if CHIPS.search(n)]
        generic = [n for n in names if re.match(r'_?_?Cail_?(Uvd|Vce|UVD|VCE)', n.lstrip('_'))]
        print(f'   UVD/VCE symbols: {len(names)}; naming a GCN 2 chip: {len(chip)}; generic CAIL entry points: {len(generic)}')
        for n in chip[:80]:
            print(f'      {n}')
        for n in generic[:40]:
            print(f'      {n}')
        for fw in a.fw:
            n, total, span = fw_presence(h.d, fw)
            print(f'   {Path(fw).name}: {n}/{total} 64-byte chunks' + (f', {span[0]:#x}-{span[1]:#x}' if span else ''))


if __name__ == '__main__':
    main()

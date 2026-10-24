# Generate kern_sigs.hpp: masked byte signatures for the HWLibs functions CarrizoRed uses, unique (or the same k-th
# match) in the symbolised reference binary and in every stripped target, plus RIP-relative references to data symbols.
# usage: siggen.py <kern_cr.cpp> <out.hpp> <ref HWLibs> <target HWLibs>...
# e.g. ref = the 15.8.1 KDK's AMDRadeonX4000HWLibs, targets = the stripped ones of older KDKs (scripts/extract_kdk.py).
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from macho import MachO

HEADER = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'siggen_header.txt'), encoding='utf-8').read()
src = open(sys.argv[1], encoding='utf-8').read()
out = sys.argv[2]
ref = MachO(sys.argv[3])
tgts = [MachO(p) for p in sys.argv[4:]]
names = sorted(s for s in set(re.findall(r'"(_[A-Za-z_][A-Za-z0-9_]+)"', src)) if s in ref.syms)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def sect(m, name):
    for seg, sn, a, size, foff, *_ in m.sects:
        if sn == name:
            return seg, a, size, foff
    return None


def text(m):
    seg, a, size, foff = sect(m, '__text')
    return m.d[foff:foff + size], a


def is_code(m, addr):
    _, a, size, _ = sect(m, '__text')
    return a <= addr < a + size


def insns(m, a, n):
    code = m.read(a, n)
    return list(md.disasm(code, a))


def masked(m, a, n):
    pat, mask = bytearray(), bytearray()
    for ins in insns(m, a, n + 16):
        b = bytearray(ins.bytes)
        k = bytearray(b'\xff' * len(b))
        if (ins.group(capstone.CS_GRP_JUMP) or ins.group(capstone.CS_GRP_CALL)) and ins.imm_size:
            for q in range(ins.imm_offset, ins.imm_offset + ins.imm_size):
                k[q] = 0
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                for q in range(ins.disp_offset, ins.disp_offset + ins.disp_size):
                    k[q] = 0
        pat += b
        mask += k
        if len(pat) >= n:
            break
    return bytes(pat[:n]), bytes(mask[:n])


def rx(pat, mask):
    return re.compile(b''.join(re.escape(bytes([p])) if k else b'.' for p, k in zip(pat, mask)), re.DOTALL)


rtext, rbase = text(ref)
ttexts = [text(t) for t in tgts]
funcs, datas, notes = [], [], []
for name in names:
    a = ref.syms[name]
    if not is_code(ref, a):
        continue
    _, end = ref.funcrange(name)
    flen = end - a
    best = None
    for n in list(range(24, min(flen, 128) + 1, 8)) + [min(flen, 128)]:
        pat, mask = masked(ref, a, n)
        r = rx(pat, mask)
        rh = [m.start() + rbase for m in r.finditer(rtext)]
        th = [[m.start() + b for m in r.finditer(t)] for t, b in ttexts]
        if a not in rh:
            break
        k = rh.index(a)
        if all(len(h) == len(rh) for h in th):
            best = (n, pat, mask, k, len(rh))
            if len(rh) == 1 or n >= 128:
                break
    if not best:
        notes.append('NO SIGNATURE: ' + name)
        continue
    n, pat, mask, k, count = best
    funcs.append((name, pat, mask, k))
    notes.append('%-50s len %3d occurrence %d of %d' % (name, n, k, count))
    # data references from this function
    for ins in insns(ref, a, min(flen, 0x800)):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address + ins.size + op.mem.disp
                dn = ref.addr2sym.get(target)
                if dn and dn in names and not is_code(ref, target) and dn not in [d[0] for d in datas]:
                    datas.append((dn, name, ins.address - a, ins.disp_offset, ins.size))

# verify data references in targets: same instruction bytes (masked) at the same offset
for dn, fn, off, doff, size in datas:
    notes.append('data %-30s via %s +0x%x' % (dn, fn, off))
    for t, (tt, tb) in zip(tgts, ttexts):
        f = [x for x in funcs if x[0] == fn][0]
        r = rx(f[1], f[2])
        hits = [m.start() + tb for m in r.finditer(tt)]
        fa = hits[f[3]]
        rb = ref.read(ref.syms[fn] + off, size)
        tbytes = t.read(fa + off, size)
        same = rb[:doff] == tbytes[:doff]
        notes.append('   target: function at %x, instruction %s' % (fa, 'same' if same else 'DIFFERENT'))

with open(out, 'w', newline='\n') as f:
    f.write(HEADER)
    f.write('#pragma once\n#include <stdint.h>\n#include <stddef.h>\n\n')
    f.write('struct HwlibsSignature {\n\tconst char *symbol;\n\tconst uint8_t *pattern, *mask;\n\tuint32_t size, occurrence;\n};\n\n')
    f.write('struct HwlibsDataRef {\n\tconst char *symbol, *function;\n\tuint32_t offset, dispOffset, size;  // the instruction in the function\n};\n\n')
    for i, (name, pat, mask, k) in enumerate(funcs):
        f.write('static const uint8_t hwsigP%d[] {%s};\n' % (i, ', '.join('0x%02x' % b for b in pat)))
        f.write('static const uint8_t hwsigM%d[] {%s};\n' % (i, ', '.join('0x%02x' % b for b in mask)))
    f.write('\nstatic const HwlibsSignature hwlibsSignatures[] {\n')
    for i, (name, pat, mask, k) in enumerate(funcs):
        f.write('\t{"%s", hwsigP%d, hwsigM%d, %d, %d},\n' % (name, i, i, len(pat), k))
    f.write('};\n\nstatic const HwlibsDataRef hwlibsDataRefs[] {\n')
    for dn, fn, off, doff, size in datas:
        f.write('\t{"%s", "%s", 0x%x, %d, %d},\n' % (dn, fn, off, doff, size))
    f.write('};\n')
print('\n'.join(notes))
print('%d functions, %d data refs' % (len(funcs), len(datas)))

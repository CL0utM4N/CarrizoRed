# Match HWLibs functions from a symbolised build to a stripped, recompiled one by what the code uses rather than its
# bytes: the immediates and memory displacements (register numbers, structure offsets), the number of calls, and the
# size. Function starts in the stripped binary come from LC_FUNCTION_STARTS.
# usage: featmatch.py <kern_cr.cpp> <ref HWLibs> <target HWLibs>
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from macho import MachO

PROLOGUE = bytes([0x55, 0x48, 0x89, 0xE5])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def function_starts(m):
    d = m.d
    ncmds = struct.unpack_from('<I', d, 16)[0]
    off, text_vm = 32, None
    starts = []
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', d, off)
        if cmd == 0x19 and d[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            text_vm = struct.unpack_from('<Q', d, off + 24)[0]
        if cmd == 0x26:  # LC_FUNCTION_STARTS
            dataoff, size = struct.unpack_from('<II', d, off + 8)
            blob, addr, i = d[dataoff:dataoff + size], 0, 0
            while i < len(blob):
                v = shift = 0
                while True:
                    b = blob[i]
                    i += 1
                    v |= (b & 0x7F) << shift
                    shift += 7
                    if not b & 0x80:
                        break
                if v == 0:
                    break
                addr += v
                starts.append(addr)
        off += cs
    if starts:
        return sorted(text_vm + a for a in starts)
    # none (stripped kexts): every push rbp; mov rbp, rsp in __text
    t = [x for x in m.sects if x[1] == '__text'][0]
    blob = d[t[4]:t[4] + t[3]]
    out, i = [], blob.find(PROLOGUE)
    while i >= 0:
        out.append(t[2] + i)
        i = blob.find(PROLOGUE, i + 1)
    return out


def features(m, a, end):
    code = m.read(a, min(end - a, 0x4000)) or b''
    consts, calls, n = [], 0, 0
    for ins in md.disasm(code, a):
        n += 1
        if ins.group(capstone.CS_GRP_CALL):
            calls += 1
            continue
        if ins.group(capstone.CS_GRP_JUMP):
            continue
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM and abs(op.imm) >= 0x10:
                consts.append(op.imm & 0xFFFFFFFF)
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_RIP and op.mem.base not in (
                    capstone.x86.X86_REG_RBP, capstone.x86.X86_REG_RSP) and op.mem.disp >= 0x10:
                consts.append(op.mem.disp)
    return set(consts), calls, n


def score(fa, fb):
    ca, kl_a, na = fa
    cb, kl_b, nb = fb
    if not ca and not cb:
        return 0
    j = len(ca & cb) / len(ca | cb)
    size = min(na, nb) / max(na, nb) if max(na, nb) else 0
    calls = min(kl_a, kl_b) / max(kl_a, kl_b) if max(kl_a, kl_b) else 1
    return j * 0.7 + size * 0.2 + calls * 0.1


src = open(sys.argv[1], encoding='utf-8').read()
ref, tgt = MachO(sys.argv[2]), MachO(sys.argv[3])
names = sorted(s for s in set(re.findall(r'"(_[A-Za-z_][A-Za-z0-9_]+)"', src)) if s in ref.syms)
text = [s for s in ref.sects if s[1] == '__text'][0]
names = [n for n in names if text[2] <= ref.syms[n] < text[2] + text[3]]
starts = function_starts(tgt)
print('target: %d function starts' % len(starts))
tfeat = [features(tgt, a, b) for a, b in zip(starts, starts[1:] + [starts[-1] + 0x400])]
results = {}
for name in names:
    a, end = ref.funcrange(name)
    f = features(ref, a, end)
    sc = sorted(((score(f, t), i) for i, t in enumerate(tfeat)), reverse=True)
    results[name] = (a, sc[:40])
anchors = []
for name, (a, sc) in results.items():
    (s1, i1), (s2, i2) = sc[0], sc[1]
    if s1 >= 0.6 and s1 - s2 >= 0.1:
        anchors.append((a, starts[i1], name))
anchors.sort()
final = {}
for name, (a, sc) in sorted(results.items(), key=lambda kv: kv[1][0]):
    (s1, i1), (s2, i2) = sc[0], sc[1]
    if s1 >= 0.6 and s1 - s2 >= 0.1:
        final[name] = (starts[i1], 'unique %.2f' % s1)
        continue
    near = min(anchors, key=lambda x: abs(x[0] - a))
    pred = a + (near[1] - near[0])
    cands = [(abs(starts[i] - pred), starts[i], s) for s, i in sc if s >= s1 - 0.05 and s >= 0.5]
    cands.sort()
    if cands and cands[0][0] < 0x600 and (len(cands) == 1 or cands[1][0] > 4 * max(cands[0][0], 0x40)):
        final[name] = (cands[0][1], 'by position %.2f, %d from predicted (anchor %s)' % (cands[0][2], cands[0][0], near[2][:30]))
    else:
        final[name] = (None, 'UNRESOLVED best %.2f, candidates %s' % (s1, [(hex(c[1]), c[0]) for c in cands[:4]]))
ok = 0
for name in names:
    addr, why = final[name]
    ok += addr is not None
    print('%-46s %-8s %s' % (name[:46], hex(addr) if addr else '-', why))
print('%d/%d resolved' % (ok, len(names)))

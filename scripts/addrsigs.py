# Signatures for functions at known addresses in a stripped binary (e.g. from featmatch.py): the shortest masked prefix
# (<= 128 bytes) that is unique in its __text, else the k-th match at 128 bytes.
# usage: addrsigs.py <binary> <addresses.txt: "symbol 0xaddr" per line> -> lines "symbol hexpattern hexmask occurrence"
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capstone
from macho import MachO

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
m = MachO(sys.argv[1])
t = [s for s in m.sects if s[1] == '__text'][0]
text, base = m.d[t[4]:t[4] + t[3]], t[2]


def masked(a, n):
    pat, mask = bytearray(), bytearray()
    for ins in md.disasm(m.read(a, n + 16), a):
        b, k = bytearray(ins.bytes), bytearray(b'\xff' * ins.size)
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


for line in open(sys.argv[2]):
    name, addr = line.split()
    a = int(addr, 16)
    best = None
    for n in range(24, 129, 8):
        pat, mask = masked(a, n)
        r = re.compile(b''.join(re.escape(bytes([p])) if k else b'.' for p, k in zip(pat, mask)), re.DOTALL)
        hits = [x.start() + base for x in r.finditer(text)]
        best = (pat, mask, hits.index(a), len(hits))
        if len(hits) == 1:
            break
    print(name, best[0].hex(), best[1].hex(), best[2], best[3])

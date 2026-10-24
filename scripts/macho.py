# Minimal Mach-O reader (x86_64, fat slices): sections, symbols, relocations, disassembly via capstone.
import struct, sys, re, bisect
import capstone

class MachO:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        if struct.unpack_from('>I', self.d, 0)[0] == 0xcafebabe:  # fat: take the x86_64 slice
            n = struct.unpack_from('>I', self.d, 4)[0]
            for i in range(n):
                cpu, sub, off, size, align = struct.unpack_from('>iiIII', self.d, 8 + 20 * i)
                if cpu == 0x01000007:
                    self.d = self.d[off:off + size]
                    break
        d = self.d
        magic, cpu, sub, ftype, ncmds, sz, flags, _ = struct.unpack_from('<IiiIIIII', d, 0)
        assert magic == 0xfeedfacf
        off = 32
        self.sects = []
        self.syms = {}
        self.addr2sym = {}
        self.relocs = {}  # address -> symbol name (external)
        symoff = None
        for _ in range(ncmds):
            cmd, cs = struct.unpack_from('<II', d, off)
            if cmd == 0x19:
                segname = d[off+8:off+24].rstrip(b'\0').decode()
                nsects = struct.unpack_from('<I', d, off+64)[0]
                so = off + 72
                for i in range(nsects):
                    sn = d[so:so+16].rstrip(b'\0').decode()
                    addr, size, foff, align, reloff, nreloc, fl = struct.unpack_from('<QQIIIII', d, so+32)
                    self.sects.append((segname, sn, addr, size, foff, reloff, nreloc))
                    so += 80
            elif cmd == 0x2:
                symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', d, off+8)
            elif cmd == 0xb:
                dys = struct.unpack_from('<18I', d, off+8)
                self.extreloff, self.nextrel = dys[14], dys[15]
            off += cs
        self.symnames = []
        for i in range(nsyms):
            strx, ntype, nsect, desc, val = struct.unpack_from('<IBBHQ', d, symoff + i*16)
            name = d[stroff+strx:d.index(b'\0', stroff+strx)].decode(errors='replace')
            self.symnames.append(name)
            if nsect and not (ntype & 0xe0):
                self.syms.setdefault(name, val)
                self.addr2sym.setdefault(val, name)
        self.sorted = sorted(self.addr2sym)
        # external relocations
        for i in range(getattr(self, 'nextrel', 0)):
            addr, info = struct.unpack_from('<iI', d, self.extreloff + i*8)
            symnum = info & 0xffffff
            ext = (info >> 27) & 1
            if ext:
                self.relocs[addr] = self.symnames[symnum]
        for s in self.sects:
            for i in range(s[6]):
                addr, info = struct.unpack_from('<iI', d, s[5] + i*8)
                if (info >> 27) & 1:
                    self.relocs[s[2] + addr] = self.symnames[info & 0xffffff]

    def read(self, addr, n):
        for seg, sn, a, size, foff, *_ in self.sects:
            if a <= addr < a + size:
                return self.d[foff + addr - a: foff + addr - a + n]
        return None

    def symat(self, addr):
        i = bisect.bisect_right(self.sorted, addr) - 1
        if i < 0: return '?'
        a = self.sorted[i]
        return self.addr2sym[a] + ('' if a == addr else '+0x%x' % (addr - a))

    def funcrange(self, name):
        a = self.syms[name]
        i = bisect.bisect_right(self.sorted, a)
        end = self.sorted[i] if i < len(self.sorted) else a + 0x1000
        return a, end

    def disasm(self, name, maxlen=0x4000):
        a, end = self.funcrange(name)
        end = min(end, a + maxlen)
        code = self.read(a, end - a)
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        out = []
        for ins in md.disasm(code, a):
            note = ''
            for k in range(ins.address + 1, ins.address + ins.size):
                if k in self.relocs:
                    note = '  ; ' + self.relocs[k]
            if not note and ins.mnemonic in ('call', 'jmp') and ins.op_str.startswith('0x'):
                note = '  ; ' + self.symat(int(ins.op_str, 16))
            if not note and 'rip' in ins.op_str:
                m = re.search(r'rip \+ (0x[0-9a-f]+)', ins.op_str)
                if m:
                    note = '  ; ' + self.symat(ins.address + ins.size + int(m.group(1), 16))
            out.append('%x: %-8s %s%s' % (ins.address, ins.mnemonic, ins.op_str, note))
        return out

if __name__ == '__main__':
    m = MachO(sys.argv[1])
    if sys.argv[2] == 'sym':
        for n in sorted(m.syms):
            if re.search(sys.argv[3], n):
                print('%x %s' % (m.syms[n], n))
    elif sys.argv[2] == 'dis':
        print('\n'.join(m.disasm(sys.argv[3], int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x4000)))
    elif sys.argv[2] == 'callers':
        # find calls to a given external/internal symbol name regex
        md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        pat = re.compile(sys.argv[3])
        hits = set()
        for addr, nm in m.relocs.items():
            if pat.search(nm):
                hits.add(m.symat(addr).split('+')[0])
        for n, a in m.syms.items():
            pass
        for h in sorted(hits): print(h)

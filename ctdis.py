"""Tiny static-analysis helper for CT_MirrorsEdge.dll (capstone + pefile)."""
import pickle, os, re, struct, sys, bisect
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_OP_IMM, X86_REG_RIP

# Path to the ORIGINAL CT_MirrorsEdge.dll (2018-02-10), not the port. Set
# CTDIS_DLL, or put it in the folder above this one (where the original
# tools' release unpacks).
DLL = os.environ.get("CTDIS_DLL", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "CT_MirrorsEdge.dll"))
CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ctdis_cache.pkl")


class Bin:
    def __init__(self, path=DLL):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = bytearray(self.pe.get_memory_mapped_image())
        self.sections = [(s.Name.rstrip(b"\0").decode(), self.base + s.VirtualAddress,
                          self.base + s.VirtualAddress + max(s.Misc_VirtualSize, s.SizeOfRawData))
                         for s in self.pe.sections]
        self.funcs = []  # (start, end)
        for e in self.pe.DIRECTORY_ENTRY_EXCEPTION:
            self.funcs.append((self.base + e.struct.BeginAddress, self.base + e.struct.EndAddress))
        self.funcs.sort()
        self.func_starts = [f[0] for f in self.funcs]
        self.imports = {}
        for d in getattr(self.pe, "DIRECTORY_ENTRY_IMPORT", []):
            for imp in d.imports:
                name = imp.name.decode() if imp.name else "ord%d" % imp.ordinal
                self.imports[imp.address] = d.dll.decode() + "!" + name
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True
        self._load_or_build()

    # ---------- memory helpers ----------
    def sec_of(self, va):
        for n, a, b in self.sections:
            if a <= va < b:
                return n
        return None

    def read(self, va, n):
        o = va - self.base
        return bytes(self.img[o:o + n])

    def q(self, va): return struct.unpack("<Q", self.read(va, 8))[0]
    def d(self, va): return struct.unpack("<I", self.read(va, 4))[0]
    def f(self, va): return struct.unpack("<f", self.read(va, 4))[0]

    def cstr(self, va, maxlen=200):
        b = self.read(va, maxlen)
        i = b.find(b"\0")
        b = b[:i if i >= 0 else maxlen]
        if len(b) >= 2 and all(32 <= c < 127 or c in (9, 10, 13) for c in b):
            return b.decode()
        # utf-16
        b = self.read(va, maxlen * 2)
        try:
            s = b.decode("utf-16le")
            s = s[:s.index("\0")] if "\0" in s else s
            if len(s) >= 2 and all(32 <= ord(c) < 127 for c in s):
                return "L" + s
        except Exception:
            pass
        return None

    def func_of(self, va):
        i = bisect.bisect_right(self.func_starts, va) - 1
        if i >= 0 and self.funcs[i][0] <= va < self.funcs[i][1]:
            return self.funcs[i]
        return None

    # ---------- disassembly ----------
    def _load_or_build(self):
        st = os.stat(DLL).st_mtime
        if os.path.exists(CACHE):
            with open(CACHE, "rb") as fh:
                c = pickle.load(fh)
            if c.get("mtime") == st:
                self.insns, self.xrefs, self.calls = c["insns"], c["xrefs"], c["calls"]
                self.insn_addrs = sorted(self.insns)
                return
        self.insns = {}   # addr -> (size, mnem, op_str, ref_target or None)
        self.xrefs = {}   # target -> [insn addr]
        self.calls = {}   # target -> [insn addr]
        seen = set()
        for a, b in self.funcs:
            if a in seen:
                continue
            seen.add(a)
            code = self.read(a, b - a)
            for ins in self.md.disasm(code, a):
                ref = None
                for op in ins.operands:
                    if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                        ref = ins.address + ins.size + op.mem.disp
                    elif op.type == X86_OP_IMM and ins.mnemonic in ("call", "jmp") or (
                            op.type == X86_OP_IMM and ins.mnemonic.startswith("j")):
                        ref = op.imm
                self.insns[ins.address] = (ins.size, ins.mnemonic, ins.op_str, ref)
                if ref is not None:
                    self.xrefs.setdefault(ref, []).append(ins.address)
                    if ins.mnemonic == "call":
                        self.calls.setdefault(ref, []).append(ins.address)
        self.insn_addrs = sorted(self.insns)
        with open(CACHE, "wb") as fh:
            pickle.dump({"mtime": st, "insns": self.insns, "xrefs": self.xrefs, "calls": self.calls}, fh)

    def annotate(self, ref):
        if ref is None:
            return ""
        if ref in self.imports:
            return "  ; " + self.imports[ref]
        sec = self.sec_of(ref)
        if sec in (".rdata", ".data"):
            s = self.cstr(ref)
            if s:
                return "  ; %r" % s
            return "  ; [%s %x]" % (sec, ref)
        if sec == ".text":
            fn = self.func_of(ref)
            if fn and fn[0] == ref:
                return "  ; sub_%x" % ref
        return ""

    def dis_range(self, a, b, out=sys.stdout):
        i = bisect.bisect_left(self.insn_addrs, a)
        while i < len(self.insn_addrs) and self.insn_addrs[i] < b:
            ad = self.insn_addrs[i]
            size, m, ops, ref = self.insns[ad]
            print("%x: %-6s %s%s" % (ad, m, ops, self.annotate(ref)), file=out)
            i += 1

    def dis_func(self, va, out=sys.stdout):
        fn = self.func_of(va)
        if not fn:
            print("no func for %x" % va, file=out)
            return
        print("==== sub_%x .. %x  (%d bytes)" % (fn[0], fn[1], fn[1] - fn[0]), file=out)
        self.dis_range(fn[0], fn[1], out)

    def find_bytes(self, needle):
        res = []
        start = 0
        while True:
            i = self.img.find(needle, start)
            if i < 0:
                break
            res.append(self.base + i)
            start = i + 1
        return res

    def find_str(self, s, wide=False):
        b = s.encode("utf-16le") if wide else s.encode()
        return [v for v in self.find_bytes(b + (b"\0\0" if wide else b"\0"))]

    def refs_to_str(self, s, wide=False):
        out = []
        for v in self.find_str(s, wide):
            out += self.xrefs.get(v, [])
        return out


if __name__ == "__main__":
    b = Bin()
    print("base %x, %d funcs, %d insns" % (b.base, len(b.funcs), len(b.insns)))
    for s in b.sections:
        print(s[0], hex(s[1]), hex(s[2]))

"""Disassemble a function of libNOVA.so by symbol name: python tools/disasm.py <substring> [count]"""
import struct, sys
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
d = open("work/lib/arm64-v8a/libNOVA.so", "rb").read()
shoff = struct.unpack_from("<Q", d, 0x28)[0]; shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
secs = {}
raw = [struct.unpack_from("<IIQQQQ", d, shoff + i * shentsize) for i in range(shnum)]
stro = raw[shstrndx][4]
for name, typ, flags, addr, off, size in raw:
    secs[d[stro + name:d.index(b"\0", stro + name)].decode()] = (addr, off, size)
so, ss = secs[".dynsym"][1:]; to = secs[".dynstr"][1]
syms = {}
for i in range(ss // 24):
    nm, info, other, shndx, val, sz = struct.unpack_from("<IBBHQQ", d, so + i * 24)
    if shndx and info & 15 == 2:
        syms[val] = (d[to + nm:d.index(b"\0", to + nm)].decode(), sz)
# Map PLT stubs to the symbols they jump to: entry i of .rela.plt belongs to the i-th 16-byte stub after the 32-byte PLT header.
plt = {}
names = []
for i in range(ss // 24):
    nm = struct.unpack_from("<I", d, so + i * 24)[0]
    names.append(d[to + nm:d.index(b"\0", to + nm)].decode())
pa, po, ps = secs[".plt"]; ra, ro, rs = secs[".rela.plt"]
for i in range(rs // 24):
    off, info, add = struct.unpack_from("<QQq", d, ro + i * 24)
    plt[pa + 32 + 16 * i] = names[info >> 32]
want = sys.argv[1]; count = int(sys.argv[2]) if len(sys.argv) > 2 else 80
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
for addr, (name, sz) in sorted(syms.items()):
    if want in name:
        print(f"== {name} @ {addr:#x} size {sz}")
        n = 0
        for ins in md.disasm(d[addr:addr + max(sz, 4)], addr):   # .text is mapped at file offset == address
            tgt = ""
            if ins.mnemonic in ("bl", "b") and ins.op_str.startswith("#"):
                t = int(ins.op_str[1:], 16)
                if t in syms: tgt = "   ; " + syms[t][0][:90]
                elif t in plt: tgt = "   ; " + plt[t][:90]
            print(f"  {ins.address:x}: {ins.mnemonic:7} {ins.op_str}{tgt}")
            n += 1
            if n >= count: break

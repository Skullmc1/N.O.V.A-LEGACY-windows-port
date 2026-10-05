"""Core of the discovery prototype: loads libNOVA.so into Unicorn and routes
every imported function to a Python handler."""
import bisect
import collections
import struct

from unicorn import (UC_ARCH_ARM64, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
                     UC_MODE_ARM, Uc, UcError)
from unicorn import arm64_const as A

BASE = 0x40000000
STUBS = 0x10000000
STUBS_SIZE = 0x100000
STACK = 0x70000000
STACK_SIZE = 0x800000
TLS = 0x78000000
HEAP = 0x80000000
HEAP_SIZE = 0x30000000
PAGE = 0x1000
RET = b"\xc0\x03\x5f\xd6"

R_ABS64, R_GLOB_DAT, R_JUMP_SLOT, R_RELATIVE = 257, 1025, 1026, 1027

IMPORTS = {}      # name -> handler(emu) for libc/android/gl imports
DATA_INIT = {}    # name -> init(emu, addr) for imported data symbols
FALLBACKS = []    # (name prefix, handler) for whole families such as gl*


def imp(*names):
    def deco(fn):
        for n in names:
            IMPORTS[n] = fn
        return fn
    return deco


def align_up(v, a=PAGE):
    return (v + a - 1) & ~(a - 1)


class Emu:
    def __init__(self, so_path):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.image = open(so_path, "rb").read()
        self.stub_names = []          # index -> name
        self.stub_fns = []            # index -> python handler
        self.stub_addr = {}           # name -> address
        self.calls = collections.Counter()
        self.recent = collections.deque(maxlen=40)   # last import calls, for fault reports
        self.unimplemented = collections.Counter()
        self.heap_top = HEAP
        self.sizes = {}
        self.free_lists = {}
        self.sched = None
        self.nested = 0              # depth of callback() runs
        self.stub_done_pc = None     # stub whose handler finished but whose `ret` may not have run
        self.exports = {}
        self.trace = False

        uc = self.uc
        uc.mem_map(STUBS, STUBS_SIZE)
        uc.mem_map(STACK, STACK_SIZE)
        uc.mem_map(TLS, PAGE)
        uc.mem_map(HEAP, HEAP_SIZE)
        uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 3 << 20)   # enable FP/SIMD
        uc.reg_write(A.UC_ARM64_REG_TPIDR_EL0, TLS + 0x100)
        uc.mem_write(TLS + 0x100 + 0x28, struct.pack("<Q", 0x5AFE5AFE5AFE5AFE))  # stack guard
        uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=STUBS, end=STUBS + STUBS_SIZE)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmapped)

        self.exit_addr = self.stub("<exit>", lambda e: e.uc.emu_stop())
        self.cb_exit = self.stub("<callback-exit>", lambda e: e.uc.emu_stop())
        # Thunk: call x16 with the current arguments, then return 0 to x17.
        self.thunk_call = self.code(
            "fe0311aa" "fd7bbfa9" "00023fd6" "fd7bc1a8" "000080d2" "c0035fd6")
        self._load()

    # ---- registers / memory -------------------------------------------
    @staticmethod
    def _xreg(n):
        # x29 and x30 are not contiguous with x0..x28 in Unicorn's register enum.
        return A.UC_ARM64_REG_X29 if n == 29 else A.UC_ARM64_REG_X30 if n == 30 else A.UC_ARM64_REG_X0 + n

    def x(self, n):
        return self.uc.reg_read(self._xreg(n))

    def setx(self, n, v):
        self.uc.reg_write(self._xreg(n), v & 0xFFFFFFFFFFFFFFFF)

    def sx(self, n):
        v = self.x(n) & 0xFFFFFFFF
        return v - (1 << 32) if v & 0x80000000 else v

    def d(self, n):
        v = self.uc.reg_read(A.UC_ARM64_REG_D0 + n)
        return v if isinstance(v, float) else struct.unpack("<d", struct.pack("<Q", v))[0]

    def setd(self, n, f):
        self.uc.reg_write(A.UC_ARM64_REG_D0 + n, struct.unpack("<Q", struct.pack("<d", f))[0])

    def s(self, n):
        v = self.uc.reg_read(A.UC_ARM64_REG_S0 + n)
        return v if isinstance(v, float) else struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0]

    def sets(self, n, f):
        self.uc.reg_write(A.UC_ARM64_REG_S0 + n, struct.unpack("<I", struct.pack("<f", f))[0])

    @property
    def sp(self):
        return self.uc.reg_read(A.UC_ARM64_REG_SP)

    @property
    def lr(self):
        return self.uc.reg_read(A.UC_ARM64_REG_X30)

    @property
    def pc(self):
        return self.uc.reg_read(A.UC_ARM64_REG_PC)

    def read(self, addr, n):
        return bytes(self.uc.mem_read(addr, n))

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def u64(self, addr):
        return struct.unpack("<Q", self.read(addr, 8))[0]

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def w64(self, addr, v):
        self.write(addr, struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))

    def w32(self, addr, v):
        self.write(addr, struct.pack("<I", v & 0xFFFFFFFF))

    def cbytes(self, addr, limit=1 << 20):
        if not addr:
            return b""
        out = bytearray()
        while len(out) < limit:
            chunk = self.read(addr + len(out), 64 - ((addr + len(out)) & 63))
            i = chunk.find(b"\0")
            if i >= 0:
                return bytes(out + chunk[:i])
            out += chunk
        return bytes(out)

    def cstr(self, addr):
        return self.cbytes(addr).decode("utf-8", "replace")

    # ---- guest heap (bump allocator; free is a no-op for now) ----------
    @staticmethod
    def _size_class(n):
        n = max(n, 16)
        step = 16 if n <= 1024 else 256 if n <= 65536 else PAGE
        return align_up(n, step)

    def malloc(self, n, align=16):
        cap = self._size_class(n)
        bucket = self.free_lists.get(cap)
        if bucket and align <= 16:
            addr = bucket.pop()
        else:
            addr = align_up(self.heap_top, align)
            self.heap_top = addr + cap
            if self.heap_top > HEAP + HEAP_SIZE:
                raise MemoryError(f"guest heap exhausted: request {n:#x}, heap used "
                                  f"{(addr - HEAP) / 1e6:.0f} MB, caller {self.sym(self.lr)}")
        self.sizes[addr] = cap
        return addr

    def capacity(self, addr):
        return self.sizes.get(addr, 0)

    def free(self, addr):
        cap = self.sizes.pop(addr, None)
        if cap:
            self.free_lists.setdefault(cap, []).append(addr)

    def alloc_bytes(self, data):
        p = self.malloc(len(data))
        self.write(p, data)
        return p

    def alloc_cstr(self, s):
        return self.alloc_bytes((s.encode() if isinstance(s, str) else s) + b"\0")

    # ---- stubs ---------------------------------------------------------
    def stub(self, name, fn):
        idx = len(self.stub_names)
        addr = STUBS + idx * 4
        self.uc.mem_write(addr, RET)
        self.stub_names.append(name)
        self.stub_fns.append(fn)
        self.stub_addr[name] = addr
        return addr

    def code(self, hexwords):
        """Place raw guest code after the stub table (not hooked by name)."""
        data = bytes.fromhex(hexwords)
        addr = STUBS + STUBS_SIZE - PAGE - 0x100 * len(getattr(self, "_codes", []))
        self._codes = getattr(self, "_codes", []) + [addr]
        self.uc.mem_write(addr, data)
        return addr

    def _on_stub(self, uc, addr, size, _):
        idx = (addr - STUBS) // 4
        if idx >= len(self.stub_names):
            return
        name = self.stub_names[idx]
        self.calls[name] += 1
        self.recent.append((name, self.x(0), self.x(1), self.lr))
        if self.trace:
            print(f"  [{name}] x0={self.x(0):#x} x1={self.x(1):#x} x2={self.x(2):#x} lr={self.sym(self.lr)}")
        ret = self.stub_fns[idx](self)
        if ret is not None:
            self.setx(0, int(ret))
        self.stub_done_pc = addr
        if self.sched is not None and not self.nested:
            self.sched.maybe_preempt()

    def _missing(self, name):
        def fn(e):
            if not e.unimplemented[name]:
                print(f"  !! unimplemented import {name}  (from {e.sym(e.lr)})")
            e.unimplemented[name] += 1
            return 0
        return fn

    def _on_unmapped(self, uc, access, addr, size, value, _):
        print(f"\n** unmapped memory access at {addr:#x} (size {size}), pc={self.sym(self.pc)}")
        return False

    # ---- ELF loading ---------------------------------------------------
    def _load(self):
        d = self.image
        phoff = struct.unpack_from("<Q", d, 0x20)[0]
        phentsize, phnum = struct.unpack_from("<HH", d, 0x36)
        loads, dyn = [], None
        for i in range(phnum):
            t, fl, off, va, pa, fs, ms, al = struct.unpack_from("<IIQQQQQQ", d, phoff + i * phentsize)
            if t == 1:
                loads.append((off, va, fs, ms))
            elif t == 2:
                dyn = (off, fs)
        top = align_up(max(va + ms for _, va, _, ms in loads))
        self.uc.mem_map(BASE, top)
        self.image_size = top
        for off, va, fs, ms in loads:
            self.uc.mem_write(BASE + va, d[off:off + fs])

        tags = {}
        for i in range(dyn[1] // 16):
            tag, val = struct.unpack_from("<qQ", d, dyn[0] + i * 16)
            tags.setdefault(tag, val)
        symtab, strtab = tags[6], tags[5]

        def va2off(va):
            for off, v, fs, ms in loads:
                if v <= va < v + fs:
                    return off + (va - v)
            raise ValueError(hex(va))

        symo, stro = va2off(symtab), va2off(strtab)
        nsyms = (stro - symo) // 24     # .dynstr directly follows .dynsym here
        syms, funcs = [], []
        for i in range(nsyms):
            nm, info, other, shndx, val, sz = struct.unpack_from("<IBBHQQ", d, symo + i * 24)
            name = d[stro + nm:d.index(b"\0", stro + nm)].decode()
            syms.append((name, info & 15, shndx, val))
            if shndx and name:
                self.exports[name] = BASE + val
                if info & 15 == 2:
                    funcs.append((BASE + val, name))
        funcs.sort()
        self._faddr = [a for a, _ in funcs]
        self._fname = [n for _, n in funcs]

        def resolve(idx):
            name, typ, shndx, val = syms[idx]
            if shndx:
                return BASE + val
            if name in self.stub_addr:
                return self.stub_addr[name]
            if typ == 1 or name in DATA_INIT:           # imported data object
                addr = self.malloc(0x400)
                self.write(addr, b"\0" * 0x400)
                if name in DATA_INIT:
                    DATA_INIT[name](self, addr)
                else:
                    print(f"  !! imported data symbol {name} left zeroed")
                self.stub_addr[name] = addr
                return addr
            fn = IMPORTS.get(name) or next((f for p, f in FALLBACKS if name.startswith(p)), None)
            return self.stub(name, fn or self._missing(name))

        for tab, size in ((tags.get(7), tags.get(8, 0)), (tags.get(23), tags.get(2, 0))):
            if not tab:
                continue
            o = va2off(tab)
            for i in range(size // 24):
                r_off, r_info, r_add = struct.unpack_from("<QQq", d, o + i * 24)
                typ, sym = r_info & 0xFFFFFFFF, r_info >> 32
                if typ == R_RELATIVE:
                    v = BASE + r_add
                elif typ in (R_ABS64, R_GLOB_DAT, R_JUMP_SLOT):
                    v = resolve(sym) + r_add
                else:
                    raise NotImplementedError(f"relocation type {typ}")
                self.uc.mem_write(BASE + r_off, struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))

        self.init_array = [self.u64(BASE + tags[25] + i * 8) for i in range(tags.get(27, 0) // 8)]

    def sym(self, addr):
        if STUBS <= addr < STUBS + STUBS_SIZE:
            i = (addr - STUBS) // 4
            return f"stub:{self.stub_names[i]}" if i < len(self.stub_names) else f"stub+{addr - STUBS:#x}"
        if BASE <= addr < BASE + self.image_size:
            i = bisect.bisect_right(self._faddr, addr) - 1
            if i >= 0:
                return f"{self._fname[i]}+{addr - self._faddr[i]:#x}"
            return f"libNOVA+{addr - BASE:#x}"
        return f"{addr:#x}"

    # ---- running guest code -------------------------------------------
    def call(self, addr, *args, count=0):
        """Run a guest function to completion. Returns x0, or None on a fault."""
        for i, a in enumerate(args):
            self.setx(i, a)
        self.uc.reg_write(A.UC_ARM64_REG_SP, STACK + STACK_SIZE - 0x1000)
        self.uc.reg_write(A.UC_ARM64_REG_X30, self.exit_addr)
        try:
            self.uc.emu_start(addr, self.exit_addr + 4, count=count)
        except UcError as err:
            print(f"\n** CPU fault: {err} at pc={self.sym(self.pc)}")
            self.backtrace()
            return None
        if self.pc != self.exit_addr and self.pc != self.exit_addr + 4:
            print(f"\n** stopped at pc={self.sym(self.pc)} (instruction budget or stop request)")
            self.backtrace()
            return None
        return self.x(0)

    def callback(self, addr, *args):
        """Call a guest function from inside an import handler (nested run)."""
        ctx = self.uc.context_save()
        for i, a in enumerate(args):
            self.setx(i, a)
        self.uc.reg_write(A.UC_ARM64_REG_SP, (self.sp - 0x200) & ~0xF)
        self.setx(30, self.cb_exit)
        self.nested += 1
        try:
            self.uc.emu_start(addr, 0)
        finally:
            self.nested -= 1
        r = self.x(0)
        self.uc.context_restore(ctx)
        return r

    def backtrace(self, depth=12):
        for name, x0, x1, lr in list(self.recent)[-12:]:
            print(f"   recent: {name}({x0:#x}, {x1:#x}) from {self.sym(lr)}")
        print(f"   lr={self.sym(self.lr)}")
        fp = self.x(29)
        for _ in range(depth):
            if not (STACK <= fp < STACK + STACK_SIZE or HEAP <= fp < self.heap_top):
                break
            fp, ret = self.u64(fp), self.u64(fp + 8)
            print(f"   <- {self.sym(ret)}")

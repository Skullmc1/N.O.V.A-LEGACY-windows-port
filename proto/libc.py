"""Python stand-ins for the bionic libc functions libNOVA.so imports.
Only what startup needs; anything missing is reported by name at runtime."""
import math
import os
import re
import struct
import time
from pathlib import Path

from emu import DATA_INIT, PAGE, align_up, imp

ROOT = Path(__file__).resolve().parent.parent
FS = ROOT / "work" / "fs"            # sandbox for the game's writable files
PKG = "com.gameloft.android.ANMP.GloftNOHM"

FAKE_FILES = {
    "/proc/cpuinfo": b"Processor\t: AArch64 Processor rev 4 (aarch64)\nprocessor\t: 0\nBogoMIPS\t: 38.40\n"
                     b"Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\nCPU implementer\t: 0x51\n"
                     b"CPU architecture: 8\nCPU variant\t: 0xa\nCPU part\t: 0x801\nCPU revision\t: 4\n\n"
                     b"Hardware\t: Qualcomm Technologies, Inc MSM8998\n",
    "/proc/meminfo": b"MemTotal:        5856780 kB\nMemFree:         2856780 kB\nMemAvailable:    3856780 kB\n",
    "/sys/devices/system/cpu/present": b"0-3\n",
    "/sys/devices/system/cpu/possible": b"0-3\n",
}


CWD = ["/"]
APK_PATH = f"/data/app/{PKG}-1/base.apk"
LIB_PATH = f"/data/app/{PKG}-1/lib/arm64/libNOVA.so"
# Guest paths that map to specific real files (the genuine APK and library).
HOST_FILES = {APK_PATH: next(ROOT.glob("*.apk")), LIB_PATH: ROOT / "work" / "lib" / "arm64-v8a" / "libNOVA.so"}
for _d in ("data/files", "data/cache", "sdcard", "sdcard_app/files"):
    (FS / _d).mkdir(parents=True, exist_ok=True)


def host_path(guest):
    """Map a guest path into the sandbox, or return None if it does not exist there."""
    g = guest.replace(chr(92), "/")
    if not g.startswith("/"):
        g = CWD[0].rstrip("/") + "/" + g
    if g in HOST_FILES:
        return HOST_FILES[g]
    for prefix, sub in ((f"/data/data/{PKG}", "data"), (f"/data/user/0/{PKG}", "data"),
                        (f"/sdcard/Android/data/{PKG}", "sdcard_app"),
                        (f"/storage/emulated/0/Android/data/{PKG}", "sdcard_app"),
                        ("/sdcard", "sdcard"), ("/storage/emulated/0", "sdcard")):
        if g.startswith(prefix):
            return FS / sub / g[len(prefix):].lstrip("/")
    return None


# ---- printf-family formatting -----------------------------------------
class RegArgs:
    """Variadic arguments passed in registers then on the stack (AAPCS64)."""
    def __init__(self, e, first_gp):
        self.e, self.gp, self.fp, self.stack = e, first_gp, 0, e.sp

    def _stack(self):
        v = self.e.u64(self.stack)
        self.stack += 8
        return v

    def int(self):
        if self.gp < 8:
            self.gp += 1
            return self.e.x(self.gp - 1)
        return self._stack()

    def double(self):
        if self.fp < 8:
            self.fp += 1
            return self.e.d(self.fp - 1)
        return struct.unpack("<d", struct.pack("<Q", self._stack()))[0]


class VaList:
    def __init__(self, e, ptr):
        self.e = e
        self.stack, self.gr_top, self.vr_top = e.u64(ptr), e.u64(ptr + 8), e.u64(ptr + 16)
        self.gr_offs, self.vr_offs = struct.unpack("<ii", e.read(ptr + 24, 8))

    def _stack(self):
        v = self.e.u64(self.stack)
        self.stack += 8
        return v

    def int(self):
        if self.gr_offs < 0:
            v = self.e.u64(self.gr_top + self.gr_offs)
            self.gr_offs += 8
            return v
        return self._stack()

    def double(self):
        if self.vr_offs < 0:
            v = self.e.u64(self.vr_top + self.vr_offs)
            self.vr_offs += 16
        else:
            v = self._stack()
        return struct.unpack("<d", struct.pack("<Q", v))[0]


FMT = re.compile(r"%([-+ #0]*)(\*|\d+)?(?:\.(\*|\d+))?(hh|h|ll|l|z|j|t|L)?([diouxXcspfFeEgG%n])")


def signed(v, bits):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


def format_c(e, fmt, args):
    def rep(m):
        flags, width, prec, length, conv = m.groups()
        if conv == "%":
            return "%"
        if width == "*":
            width = str(signed(args.int(), 32))
        if prec == "*":
            prec = str(signed(args.int(), 32))
        spec = "%" + flags + (width or "") + ("." + prec if prec is not None else "")
        bits = 64 if length in ("l", "ll", "z", "j", "t") else 32
        try:
            if conv in "di":
                return (spec + "d") % signed(args.int(), bits)
            if conv in "ouxX":
                return (spec + conv.replace("u", "d")) % (args.int() & ((1 << bits) - 1))
            if conv == "c":
                return (spec + "c") % chr(args.int() & 0xFF)
            if conv == "s":
                p = args.int()
                return (spec + "s") % (e.cstr(p) if p else "(null)")
            if conv == "p":
                return "%#x" % args.int()
            if conv == "n":
                args.int()
                return ""
            return (spec + conv) % args.double()
        except (TypeError, ValueError):
            return m.group(0)
    return FMT.sub(rep, fmt)


def emit(e, dst, size, text):
    data = text.encode("utf-8", "replace")
    if dst and size:
        e.write(dst, data[:size - 1] + b"\0")
    return len(data)


@imp("snprintf")
def _snprintf(e):
    return emit(e, e.x(0), e.x(1), format_c(e, e.cstr(e.x(2)), RegArgs(e, 3)))


@imp("sprintf")
def _sprintf(e):
    return emit(e, e.x(0), 1 << 30, format_c(e, e.cstr(e.x(1)), RegArgs(e, 2)))


@imp("vsnprintf")
def _vsnprintf(e):
    return emit(e, e.x(0), e.x(1), format_c(e, e.cstr(e.x(2)), VaList(e, e.x(3))))


@imp("vsprintf")
def _vsprintf(e):
    return emit(e, e.x(0), 1 << 30, format_c(e, e.cstr(e.x(1)), VaList(e, e.x(2))))


@imp("vasprintf")
def _vasprintf(e):
    text = format_c(e, e.cstr(e.x(1)), VaList(e, e.x(2)))
    e.w64(e.x(0), e.alloc_cstr(text))
    return len(text.encode())


@imp("printf")
def _printf(e):
    text = format_c(e, e.cstr(e.x(0)), RegArgs(e, 1))
    print("  [stdout]", text.rstrip())
    return len(text)


@imp("puts")
def _puts(e):
    print("  [stdout]", e.cstr(e.x(0)))
    return 1


@imp("fprintf")
def _fprintf(e):
    text = format_c(e, e.cstr(e.x(1)), RegArgs(e, 2))
    print("  [fprintf]", text.rstrip())
    return len(text)


@imp("vfprintf")
def _vfprintf(e):
    text = format_c(e, e.cstr(e.x(1)), VaList(e, e.x(2)))
    print("  [fprintf]", text.rstrip())
    return len(text)


@imp("__android_log_print")
def _alog(e):
    print(f"  [log {e.cstr(e.x(1))}]", format_c(e, e.cstr(e.x(2)), RegArgs(e, 3)).rstrip())
    return 0


# ---- memory -------------------------------------------------------------
@imp("malloc")
def _malloc(e):
    return e.malloc(e.x(0))


@imp("calloc")
def _calloc(e):
    n = e.x(0) * e.x(1)
    p = e.malloc(n)
    e.write(p, b"\0" * n)
    return p


@imp("realloc")
def _realloc(e):
    old, n = e.x(0), e.x(1)
    if old and n <= e.capacity(old):
        return old
    p = e.malloc(n)
    if old:
        e.write(p, e.read(old, min(e.capacity(old), n)))
        e.free(old)
    return p


@imp("memalign")
def _memalign(e):
    return e.malloc(e.x(1), max(e.x(0), 16))


@imp("free", "munmap")
def _free(e):
    e.free(e.x(0))
    return 0


@imp("mmap")
def _mmap(e):
    n = align_up(e.x(1))
    p = e.malloc(n, PAGE)
    e.write(p, b"\0" * n)
    return p


@imp("memcpy", "memmove")
def _memcpy(e):
    if e.x(2):
        e.write(e.x(0), e.read(e.x(1), e.x(2)))
    return e.x(0)


@imp("memset")
def _memset(e):
    if e.x(2):
        e.write(e.x(0), bytes([e.x(1) & 0xFF]) * e.x(2))
    return e.x(0)


def cmp(a, b):
    return (a > b) - (a < b)


@imp("memcmp")
def _memcmp(e):
    return cmp(e.read(e.x(0), e.x(2)), e.read(e.x(1), e.x(2)))


@imp("memchr")
def _memchr(e):
    i = e.read(e.x(0), e.x(2)).find(bytes([e.x(1) & 0xFF]))
    return e.x(0) + i if i >= 0 else 0


# ---- strings ------------------------------------------------------------
@imp("strlen")
def _strlen(e):
    return len(e.cbytes(e.x(0)))


@imp("strnlen")
def _strnlen(e):
    return min(len(e.cbytes(e.x(0), e.x(1))), e.x(1))


@imp("strcmp", "strcoll")
def _strcmp(e):
    return cmp(e.cbytes(e.x(0)), e.cbytes(e.x(1)))


@imp("strncmp")
def _strncmp(e):
    n = e.x(2)
    return cmp(e.cbytes(e.x(0), n)[:n], e.cbytes(e.x(1), n)[:n])


@imp("strcasecmp")
def _strcasecmp(e):
    return cmp(e.cbytes(e.x(0)).lower(), e.cbytes(e.x(1)).lower())


@imp("strncasecmp")
def _strncasecmp(e):
    n = e.x(2)
    return cmp(e.cbytes(e.x(0), n)[:n].lower(), e.cbytes(e.x(1), n)[:n].lower())


@imp("strcpy")
def _strcpy(e):
    e.write(e.x(0), e.cbytes(e.x(1)) + b"\0")
    return e.x(0)


@imp("strncpy")
def _strncpy(e):
    n = e.x(2)
    e.write(e.x(0), e.cbytes(e.x(1), n)[:n].ljust(n, b"\0"))
    return e.x(0)


@imp("strcat")
def _strcat(e):
    e.write(e.x(0) + len(e.cbytes(e.x(0))), e.cbytes(e.x(1)) + b"\0")
    return e.x(0)


@imp("strncat")
def _strncat(e):
    n = e.x(2)
    e.write(e.x(0) + len(e.cbytes(e.x(0))), e.cbytes(e.x(1), n)[:n] + b"\0")
    return e.x(0)


@imp("strdup")
def _strdup(e):
    return e.alloc_cstr(e.cbytes(e.x(0)))


@imp("strchr")
def _strchr(e):
    s, c = e.cbytes(e.x(0)), e.x(1) & 0xFF
    if c == 0:
        return e.x(0) + len(s)
    i = s.find(bytes([c]))
    return e.x(0) + i if i >= 0 else 0


@imp("strrchr")
def _strrchr(e):
    s, c = e.cbytes(e.x(0)), e.x(1) & 0xFF
    if c == 0:
        return e.x(0) + len(s)
    i = s.rfind(bytes([c]))
    return e.x(0) + i if i >= 0 else 0


@imp("strstr")
def _strstr(e):
    i = e.cbytes(e.x(0)).find(e.cbytes(e.x(1)))
    return e.x(0) + i if i >= 0 else 0


@imp("strspn")
def _strspn(e):
    s, a = e.cbytes(e.x(0)), e.cbytes(e.x(1))
    return next((i for i, c in enumerate(s) if c not in a), len(s))


@imp("strcspn")
def _strcspn(e):
    s, r = e.cbytes(e.x(0)), e.cbytes(e.x(1))
    return next((i for i, c in enumerate(s) if c in r), len(s))


@imp("strpbrk")
def _strpbrk(e):
    s, a = e.cbytes(e.x(0)), e.cbytes(e.x(1))
    i = next((i for i, c in enumerate(s) if c in a), -1)
    return e.x(0) + i if i >= 0 else 0


NUM = re.compile(rb"\s*([-+]?)(0[xX])?([0-9a-zA-Z]*)")


def parse_int(e, bits, is_signed):
    s, endp, base = e.cbytes(e.x(0), 128), e.x(1), e.sx(2)
    m = NUM.match(s)
    sign, hexp, digits = m.groups()
    if hexp and base in (0, 16):
        base = 16
    elif base == 0:
        base = 8 if digits[:1] == b"0" and len(digits) > 1 else 10
    else:
        digits = (hexp or b"")[:1] + digits if hexp else digits
    val, used = 0, 0
    for ch in digits:
        dv = int(chr(ch), 36) if chr(ch).isalnum() else 99
        if dv >= base:
            break
        val, used = val * base + dv, used + 1
    end = m.start(3) + used if used else 0
    if endp:
        e.w64(endp, e.x(0) + end)
    if sign == b"-":
        val = -val
    lim = (1 << (bits - 1)) - 1 if is_signed else (1 << bits) - 1
    return max(min(val, lim), -lim - 1 if is_signed else -lim)


for _n, _b, _s in (("strtol", 64, True), ("strtoll", 64, True), ("strtoul", 64, False), ("strtoull", 64, False)):
    imp(_n)(lambda e, b=_b, s=_s: parse_int(e, b, s))


@imp("atoi", "atol")
def _atoi(e):
    m = re.match(rb"\s*[-+]?\d+", e.cbytes(e.x(0), 64))
    return int(m.group()) if m else 0


FLOAT = re.compile(rb"\s*[-+]?(\d+\.?\d*([eE][-+]?\d+)?|\.\d+([eE][-+]?\d+)?|inf|nan)", re.I)


def parse_float(e):
    m = FLOAT.match(e.cbytes(e.x(0), 128))
    if e.x(1):
        e.w64(e.x(1), e.x(0) + (m.end() if m else 0))
    return float(m.group()) if m else 0.0


@imp("strtod", "atof")
def _strtod(e):
    if e.stub_names[(e.pc - 0x10000000) // 4] == "atof":
        e.setx(1, 0)
    e.setd(0, parse_float(e))


@imp("strtof")
def _strtof(e):
    e.sets(0, parse_float(e))


@imp("tolower")
def _tolower(e):
    c = e.sx(0)
    return c + 32 if 65 <= c <= 90 else c


@imp("toupper")
def _toupper(e):
    c = e.sx(0)
    return c - 32 if 97 <= c <= 122 else c


for _n, _f in (("isalnum", str.isalnum), ("isalpha", str.isalpha), ("isspace", str.isspace),
               ("isupper", str.isupper), ("islower", str.islower), ("iscntrl", lambda c: ord(c) < 32 or ord(c) == 127),
               ("ispunct", lambda c: c.isprintable() and not c.isalnum() and c != " "),
               ("isxdigit", lambda c: c in "0123456789abcdefABCDEF")):
    imp(_n)(lambda e, f=_f: int(0 <= e.sx(0) < 128 and bool(f(chr(e.sx(0))))))


# ---- maths --------------------------------------------------------------
def _math(fn, nargs, single):
    def h(e):
        get, put = (e.s, e.sets) if single else (e.d, e.setd)
        try:
            r = fn(*[get(i) for i in range(nargs)])
        except (ValueError, OverflowError):
            r = math.nan
        put(0, r)
    return h


for _n, _f, _a in (("sin", math.sin, 1), ("cos", math.cos, 1), ("tan", math.tan, 1), ("asin", math.asin, 1),
                   ("acos", math.acos, 1), ("atan", math.atan, 1), ("atan2", math.atan2, 2), ("exp", math.exp, 1),
                   ("exp2", lambda v: 2.0 ** v, 1), ("log", math.log, 1), ("log10", math.log10, 1),
                   ("pow", math.pow, 2), ("sqrt", math.sqrt, 1), ("fmod", math.fmod, 2), ("sinh", math.sinh, 1),
                   ("cosh", math.cosh, 1), ("tanh", math.tanh, 1)):
    imp(_n)(_math(_f, _a, False))
    imp(_n + "f")(_math(_f, _a, True))


@imp("ldexp")
def _ldexp(e):
    e.setd(0, math.ldexp(e.d(0), e.sx(0)))


@imp("ldexpf")
def _ldexpf(e):
    e.sets(0, math.ldexp(e.s(0), e.sx(0)))


@imp("frexp")
def _frexp(e):
    m, ex = math.frexp(e.d(0))
    e.w32(e.x(0), ex)
    e.setd(0, m)


@imp("modf")
def _modf(e):
    f, i = math.modf(e.d(0))
    e.write(e.x(0), struct.pack("<d", i))
    e.setd(0, f)


@imp("modff")
def _modff(e):
    f, i = math.modf(e.s(0))
    e.write(e.x(0), struct.pack("<f", i))
    e.sets(0, f)


# ---- time ---------------------------------------------------------------
T0 = time.perf_counter()


@imp("clock_gettime")
def _clock_gettime(e):
    t = time.time() if e.x(0) == 0 else time.perf_counter() - T0 + 1000.0
    e.write(e.x(1), struct.pack("<qq", int(t), int((t % 1) * 1e9)))
    return 0


@imp("gettimeofday")
def _gettimeofday(e):
    t = time.time()
    e.write(e.x(0), struct.pack("<qq", int(t), int((t % 1) * 1e6)))
    return 0


@imp("time")
def _time(e):
    t = int(time.time())
    if e.x(0):
        e.w64(e.x(0), t)
    return t


@imp("clock")
def _clock(e):
    return int((time.perf_counter() - T0) * 1e6)


# ---- misc no-ops (threads and locks live in sched.py) ---------------------
for _n in ("__cxa_atexit", "__cxa_finalize", "setlocale", "sigaction", "signal", "closelog", "openlog", "syslog",
           "fflush", "setvbuf", "umask", "srand", "chmod"):
    imp(_n)(lambda e: 0)


JMP = {}


@imp("setjmp")
def _setjmp(e):
    JMP[e.x(0)] = ([e.x(i) for i in range(19, 31)], e.sp, [e.d(i) for i in range(8, 16)])
    return 0


@imp("longjmp")
def _longjmp(e):
    from unicorn import arm64_const as A
    regs, sp, fps = JMP[e.x(0)]
    val = e.x(1) or 1
    for i, v in enumerate(regs):
        e.setx(19 + i, v)
    for i, v in enumerate(fps):
        e.setd(8 + i, v)
    e.uc.reg_write(A.UC_ARM64_REG_SP, sp)
    return val


# ---- process / environment ----------------------------------------------
ERRNO = {}


@imp("__errno")
def _errno(e):
    if "p" not in ERRNO:
        ERRNO["p"] = e.malloc(8)
        e.w64(ERRNO["p"], 0)
    return ERRNO["p"]


def set_errno(e, v):
    _errno(e)
    e.w32(ERRNO["p"], v)


@imp("sysconf")
def _sysconf(e):
    return {39: 4096, 40: 4096, 96: 4, 97: 4, 98: 1 << 20}.get(e.sx(0), -1) & 0xFFFFFFFFFFFFFFFF


@imp("getpagesize")
def _getpagesize(e):
    return 4096


@imp("getenv", "getpwuid", "localeconv_missing")
def _null(e):
    return 0


@imp("getuid", "geteuid", "getgid", "getegid")
def _uid(e):
    return 10123


@imp("__stack_chk_fail", "abort")
def _abort(e):
    name = e.stub_names[(e.pc - 0x10000000) // 4]
    print(f"\n** guest called {name}")
    e.backtrace()
    e.uc.emu_stop()


@imp("exit")
def _exit(e):
    print(f"\n** guest called exit({e.sx(0)})")
    e.uc.emu_stop()


@imp("android_set_abort_message")
def _abort_msg(e):
    print("  [abort message]", e.cstr(e.x(0)))


@imp("strerror")
def _strerror(e):
    return e.alloc_cstr(os.strerror(e.sx(0)))


@imp("qsort")
def _qsort(e):
    import functools
    base, n, size, fn = e.x(0), e.x(1), e.x(2), e.x(3)
    items = [e.read(base + i * size, size) for i in range(n)]
    a, b = e.malloc(size), e.malloc(size)

    def compare(p, q):
        e.write(a, p)
        e.write(b, q)
        return signed(e.callback(fn, a, b), 32)
    items.sort(key=functools.cmp_to_key(compare))
    e.write(base, b"".join(items))
    return 0


@imp("rand")
def _rand(e):
    import random
    return random.getrandbits(31)


@imp("newlocale", "uselocale")
def _locale(e):
    return e.stub_addr.get("__sF", 1)     # any non-null token


@imp("freelocale")
def _freelocale(e):
    return 0


@imp("__ctype_get_mb_cur_max")
def _mbmax(e):
    return 4


# ---- files --------------------------------------------------------------
FILES = {}     # FILE* / fd -> python file object
MISSING = set()  # paths already reported as missing
NEXT_FD = [100]


def open_guest(e, path, mode):
    if path in ("/dev/urandom", "/dev/random"):
        import io
        return io.BytesIO(os.urandom(1 << 16))
    if path in FAKE_FILES and "r" in mode:
        import io
        return io.BytesIO(FAKE_FILES[path])
    hp = host_path(path)
    if hp is None:
        return None
    try:
        return open(hp, mode.replace("t", "").replace("b", "") + "b")
    except OSError:
        return None


@imp("fopen")
def _fopen(e):
    path, mode = e.cstr(e.x(0)), e.cstr(e.x(1))
    f = open_guest(e, path, mode)
    if f or path not in MISSING:
        print(f"  [fopen] {path!r} mode={mode} -> {'ok' if f else 'FAIL'}")
    if not f:
        MISSING.add(path)
    if not f:
        set_errno(e, 2)
        return 0
    p = e.malloc(160)
    e.write(p, b"\0" * 160)
    FILES[p] = f
    return p


@imp("fclose")
def _fclose(e):
    f = FILES.pop(e.x(0), None)
    if f:
        f.close()
    return 0


@imp("fread")
def _fread(e):
    f, size = FILES.get(e.x(3)), e.x(1)
    if not f or not size:
        return 0
    data = f.read(size * e.x(2))
    e.write(e.x(0), data)
    return len(data) // size


@imp("fwrite")
def _fwrite(e):
    f, n = FILES.get(e.x(3)), e.x(1) * e.x(2)
    data = e.read(e.x(0), n)
    if f:
        f.write(data)
    else:
        print("  [fwrite std]", data.decode("utf-8", "replace").rstrip())
    return e.x(2)


@imp("fputs")
def _fputs(e):
    f = FILES.get(e.x(1))
    if f:
        f.write(e.cbytes(e.x(0)))
    else:
        print("  [fputs]", e.cstr(e.x(0)).rstrip())
    return 1


@imp("fputc", "putc")
def _fputc(e):
    f = FILES.get(e.x(1))
    if f:
        f.write(bytes([e.x(0) & 0xFF]))
    return e.x(0) & 0xFF


@imp("fgets")
def _fgets(e):
    f = FILES.get(e.x(2))
    line = f.readline(e.sx(1) - 1) if f else b""
    if not line:
        return 0
    e.write(e.x(0), line + b"\0")
    return e.x(0)


@imp("getc")
def _getc(e):
    f = FILES.get(e.x(0))
    c = f.read(1) if f else b""
    return c[0] if c else 0xFFFFFFFF


@imp("fseek", "fseeko")
def _fseek(e):
    f = FILES.get(e.x(0))
    if not f:
        return -1 & 0xFFFFFFFF
    f.seek(signed(e.x(1), 64), e.sx(2))
    return 0


@imp("ftell", "ftello")
def _ftell(e):
    f = FILES.get(e.x(0))
    return f.tell() if f else -1 & 0xFFFFFFFFFFFFFFFF


@imp("rewind")
def _rewind(e):
    f = FILES.get(e.x(0))
    if f:
        f.seek(0)


@imp("feof")
def _feof(e):
    f = FILES.get(e.x(0))
    if not f:
        return 1
    pos = f.tell()
    end = f.seek(0, 2)
    f.seek(pos)
    return int(pos >= end)


@imp("ferror", "clearerr")
def _ferror(e):
    return 0


def fail(e, what, path):
    print(f"  [{what}] {path!r} -> FAIL")
    set_errno(e, 2)
    return 0xFFFFFFFFFFFFFFFF


@imp("open")
def _open(e):
    path, flags = e.cstr(e.x(0)), e.sx(1)
    mode = "r" if flags & 3 == 0 else ("r+" if not flags & 0x40 else "w+")
    f = open_guest(e, path, mode)
    if not f:
        return fail(e, "open", path)
    print(f"  [open] {path!r} flags={flags:#x} -> ok")
    NEXT_FD[0] += 1
    FILES[NEXT_FD[0]] = f
    return NEXT_FD[0]


@imp("close")
def _close(e):
    f = FILES.pop(e.x(0), None)
    if f:
        f.close()
    return 0


@imp("read")
def _read(e):
    f = FILES.get(e.x(0))
    if not f:
        return 0xFFFFFFFFFFFFFFFF
    data = f.read(e.x(2))
    e.write(e.x(1), data)
    return len(data)


@imp("write")
def _write(e):
    f = FILES.get(e.x(0))
    data = e.read(e.x(1), e.x(2))
    if f:
        f.write(data)
    else:
        print(f"  [write fd={e.x(0)}]", data.decode("utf-8", "replace").rstrip())
    return e.x(2)


@imp("lseek")
def _lseek(e):
    f = FILES.get(e.x(0))
    return f.seek(signed(e.x(1), 64), e.sx(2)) if f else 0xFFFFFFFFFFFFFFFF


def stat_path(e, path, buf):
    hp = host_path(path)
    if path in FAKE_FILES:
        size, isdir = len(FAKE_FILES[path]), False
    elif hp is not None and hp.exists():
        size, isdir = hp.stat().st_size, hp.is_dir()
    else:
        return fail(e, "stat", path)
    st = bytearray(128)                       # struct stat, arm64 bionic
    struct.pack_into("<I", st, 16, (0o040755 if isdir else 0o100644))
    struct.pack_into("<q", st, 48, size)
    e.write(buf, st)
    return 0


@imp("stat")
def _stat(e):
    return stat_path(e, e.cstr(e.x(0)), e.x(1))


@imp("access")
def _access(e):
    path = e.cstr(e.x(0))
    hp = host_path(path)
    if path in FAKE_FILES or (hp is not None and hp.exists()):
        return 0
    return fail(e, "access", path)


@imp("mkdir")
def _mkdir(e):
    path = e.cstr(e.x(0))
    hp = host_path(path)
    if hp is None:
        return fail(e, "mkdir", path)
    if not hp.parent.is_dir():          # like the real mkdir: no missing parents
        return fail(e, "mkdir", path)
    hp.mkdir(exist_ok=True)
    return 0


@imp("remove", "unlink")
def _remove(e):
    hp = host_path(e.cstr(e.x(0)))
    if hp is not None and hp.is_file():
        hp.unlink()
        return 0
    return 0xFFFFFFFFFFFFFFFF


@imp("opendir")
def _opendir(e):
    print(f"  [opendir] {e.cstr(e.x(0))!r} -> FAIL")
    return 0


@imp("getaddrinfo")
def _getaddrinfo(e):
    print(f"  [net] getaddrinfo {e.cstr(e.x(0))!r} -> no network")
    return 8          # EAI_NONAME


@imp("gethostbyname")
def _gethostbyname(e):
    print(f"  [net] gethostbyname {e.cstr(e.x(0))!r} -> no network")
    return 0


@imp("dl_iterate_phdr")
def _dl_iterate_phdr(e):
    import struct as st
    from emu import BASE
    phoff = st.unpack_from("<Q", e.image, 0x20)[0]
    phnum = st.unpack_from("<H", e.image, 0x38)[0]
    info = e.alloc_bytes(st.pack("<QQQH", BASE, e.alloc_cstr("libNOVA.so"), BASE + phoff, phnum).ljust(64, b"\0"))
    return e.callback(e.x(0), info, 64, e.x(1))


for _n in ("socket", "connect", "bind", "fstat",
           "rmdir", "system"):
    def _neg(e, n=_n):
        if not e.unimplemented[n]:
            print(f"  [{n}] -> -1 (not supported yet)  from {e.sym(e.lr)}")
        e.unimplemented[n] += 1
        return 0xFFFFFFFFFFFFFFFF
    imp(_n)(_neg)


# ---- imported data objects ----------------------------------------------
def _init_ctype(e, addr):
    U, L, N, S, P, C, X, B = 1, 2, 4, 8, 16, 32, 64, 128
    tab = bytearray(257)
    for c in range(128):
        ch, f = chr(c), 0
        if ch.isupper():
            f |= U
        if ch.islower():
            f |= L
        if ch.isdigit():
            f |= N
        if ch in " \t\n\r\v\f":
            f |= S
        if c < 32 or c == 127:
            f |= C
        if ch in "abcdefABCDEF":
            f |= X
        if 33 <= c < 127 and not ch.isalnum():
            f |= P
        if c == 32:
            f |= B
        tab[c + 1] = f
    e.w64(addr, e.alloc_bytes(tab))


DATA_INIT["_ctype_"] = _init_ctype
DATA_INIT["__sF"] = lambda e, addr: None                 # stdin/stdout/stderr FILE structs, zeroed
DATA_INIT["timezone"] = lambda e, addr: None
for _i, _n in enumerate(("SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_BUFFERQUEUE")):
    DATA_INIT[_n] = lambda e, addr, i=_i: e.w64(addr, e.alloc_bytes(bytes([i + 1]) * 16))

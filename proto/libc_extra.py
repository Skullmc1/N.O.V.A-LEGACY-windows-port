"""More libc stand-ins found necessary once the game's main loop started:
scanf, working directory, wide strings and the *_l locale variants."""
import re
import struct

import libc
from emu import IMPORTS, imp
from libc import RegArgs, VaList, fail, host_path

# ---- scanf family ---------------------------------------------------------
SCAN = re.compile(r"%(\*)?(\d+)?(hh|h|ll|l|z|j|t|L)?(\[\^?\]?[^\]]*\]|[diouxXfFeEgGscpn%])")
INT_RE = {"d": r"[-+]?\d+", "u": r"[-+]?\d+", "i": r"[-+]?(?:0[xX][0-9a-fA-F]+|\d+)",
          "x": r"[-+]?(?:0[xX])?[0-9a-fA-F]+", "X": r"[-+]?(?:0[xX])?[0-9a-fA-F]+", "o": r"[-+]?[0-7]+",
          "p": r"(?:0[xX])?[0-9a-fA-F]+"}
FLOAT_RE = r"[-+]?(?:\d+\.?\d*(?:[eE][-+]?\d+)?|\.\d+(?:[eE][-+]?\d+)?|inf|nan)"
INT_SIZE = {"hh": 1, "h": 2, None: 4, "l": 8, "ll": 8, "z": 8, "j": 8, "t": 8}


def scan(e, text, fmt, args):
    pos, done, i = 0, 0, 0
    while i < len(fmt):
        ch = fmt[i]
        if ch.isspace():
            while pos < len(text) and text[pos].isspace():
                pos += 1
            i += 1
            continue
        m = SCAN.match(fmt, i) if ch == "%" else None
        if m is None:
            if pos >= len(text) or text[pos] != ch:
                break
            pos, i = pos + 1, i + 1
            continue
        i = m.end()
        skip, width, length, conv = m.groups()
        width = int(width) if width else None
        if conv == "%":
            if text[pos:pos + 1] != "%":
                break
            pos += 1
            continue
        if conv == "n":
            e.w32(args.int(), pos)
            continue
        if conv not in "c[":
            while pos < len(text) and text[pos].isspace():
                pos += 1
        chunk = text[pos:pos + width] if width else text[pos:]
        if conv in INT_RE:
            mm = re.match(INT_RE[conv], chunk)
            if not mm:
                break
            pos += mm.end()
            if not skip:
                base = {"d": 10, "u": 10, "i": 0, "o": 8}.get(conv, 16)
                size = 8 if conv == "p" else INT_SIZE[length]
                value = int(mm.group(), base) & ((1 << (8 * size)) - 1)
                e.write(args.int(), value.to_bytes(size, "little"))
        elif conv in "fFeEgG":
            mm = re.match(FLOAT_RE, chunk, re.I)
            if not mm:
                break
            pos += mm.end()
            if not skip:
                e.write(args.int(), struct.pack("<d" if length in ("l", "L") else "<f", float(mm.group())))
        else:
            if conv == "s":
                mm = re.match(r"\S+", chunk)
            elif conv == "c":
                mm = re.match(r"(?s).{%d}" % (width or 1), chunk)
            else:
                mm = re.match(conv + "+", chunk)
            if not mm:
                break
            pos += mm.end()
            if not skip:
                e.write(args.int(), mm.group().encode("utf-8") + (b"" if conv == "c" else b"\0"))
        if not skip:
            done += 1
    return done if done or pos else 0xFFFFFFFF if not text else 0


@imp("sscanf")
def _sscanf(e):
    return scan(e, e.cstr(e.x(0)), e.cstr(e.x(1)), RegArgs(e, 2))


@imp("vsscanf")
def _vsscanf(e):
    return scan(e, e.cstr(e.x(0)), e.cstr(e.x(1)), VaList(e, e.x(2)))


@imp("fscanf")
def _fscanf(e):
    f = libc.FILES.get(e.x(0))
    if not f:
        return 0xFFFFFFFF
    return scan(e, f.readline().decode("utf-8", "replace"), e.cstr(e.x(1)), RegArgs(e, 2))


# ---- working directory / file management -----------------------------------
@imp("getcwd")
def _getcwd(e):
    e.write(e.x(0), libc.CWD[0].encode() + b"\0")
    return e.x(0)


@imp("chdir")
def _chdir(e):
    path = e.cstr(e.x(0))
    hp = host_path(path)
    if hp is None:
        return fail(e, "chdir", path)
    hp.mkdir(parents=True, exist_ok=True)
    libc.CWD[0] = path
    return 0


@imp("rename")
def _rename(e):
    src, dst = host_path(e.cstr(e.x(0))), host_path(e.cstr(e.x(1)))
    if src is None or dst is None or not src.exists():
        return 0xFFFFFFFFFFFFFFFF
    src.replace(dst)
    return 0


@imp("syscall")
def _syscall(e):
    return 0


# ---- wide strings (wchar_t is 32-bit on Android) ---------------------------
def wstr(e, addr, limit=1 << 16):
    out = []
    while len(out) < limit:
        c = e.u32(addr + 4 * len(out))
        if not c:
            break
        out.append(c)
    return out


@imp("wcslen")
def _wcslen(e):
    return len(wstr(e, e.x(0)))


@imp("wmemcpy", "wmemmove")
def _wmemcpy(e):
    if e.x(2):
        e.write(e.x(0), e.read(e.x(1), 4 * e.x(2)))
    return e.x(0)


@imp("wmemset")
def _wmemset(e):
    e.write(e.x(0), struct.pack("<I", e.x(1) & 0xFFFFFFFF) * e.x(2))
    return e.x(0)


@imp("wcscmp", "wcscoll")
def _wcscmp(e):
    a, b = wstr(e, e.x(0)), wstr(e, e.x(1))
    return (a > b) - (a < b)


# ---- locale-suffixed and wide ctype variants -------------------------------
@imp("isdigit")
def _isdigit(e):
    return int(48 <= e.sx(0) <= 57)


for _name in ("isdigit", "islower", "isupper", "isxdigit", "tolower", "toupper", "strcoll", "strtoll", "strtoull",
              "wcscoll"):
    IMPORTS[_name + "_l"] = IMPORTS[_name]
for _wide, _narrow in (("iswalpha", "isalpha"), ("iswcntrl", "iscntrl"), ("iswdigit", "isdigit"),
                       ("iswlower", "islower"), ("iswpunct", "ispunct"), ("iswspace", "isspace"),
                       ("iswupper", "isupper"), ("iswxdigit", "isxdigit"), ("towlower", "tolower"),
                       ("towupper", "toupper")):
    IMPORTS[_wide + "_l"] = IMPORTS[_wide] = IMPORTS[_narrow]
IMPORTS["iswblank_l"] = lambda e: int(e.sx(0) in (9, 32))
IMPORTS["iswprint_l"] = lambda e: int(32 <= e.sx(0) < 127)


# ---- funopen: a FILE* whose I/O is done by guest callbacks ------------------
class FunFile:
    def __init__(self, e, cookie, readfn, writefn, seekfn, closefn):
        self.e, self.cookie = e, cookie
        self.readfn, self.writefn, self.seekfn, self.closefn = readfn, writefn, seekfn, closefn
        self.buf = e.malloc(1 << 16)

    def read(self, n=-1):
        out = bytearray()
        while n < 0 or len(out) < n:
            want = (1 << 16) if n < 0 else min(n - len(out), 1 << 16)
            got = libc.signed(self.e.callback(self.readfn, self.cookie, self.buf, want), 32)
            if got <= 0:
                break
            out += self.e.read(self.buf, got)
        return bytes(out)

    def readline(self, limit=-1):
        out = bytearray()
        while limit < 0 or len(out) < limit:
            c = self.read(1)
            if not c:
                break
            out += c
            if c == b"\n":
                break
        return bytes(out)

    def write(self, data):
        if self.writefn:
            p = self.e.alloc_bytes(data)
            self.e.callback(self.writefn, self.cookie, p, len(data))
            self.e.free(p)
        return len(data)

    def seek(self, off, whence=0):
        if not self.seekfn:
            return -1
        return libc.signed(self.e.callback(self.seekfn, self.cookie, off & 0xFFFFFFFFFFFFFFFF, whence), 64)

    def tell(self):
        return self.seek(0, 1)

    def close(self):
        if self.closefn:
            self.e.callback(self.closefn, self.cookie)
        self.e.free(self.buf)


@imp("funopen")
def _funopen(e):
    p = e.malloc(160)
    e.write(p, bytes(160))
    libc.FILES[p] = FunFile(e, *(e.x(i) for i in range(5)))
    return p


# ---- epoll / eventfd / pipe: enough for the networking threads to idle ------
FAKE_FDS = [1000]


def fake_fd():
    FAKE_FDS[0] += 1
    return FAKE_FDS[0]


@imp("eventfd", "epoll_create", "epoll_create1")
def _fakefd(e):
    return fake_fd()


@imp("pipe")
def _pipe(e):
    e.write(e.x(0), struct.pack("<ii", fake_fd(), fake_fd()))
    return 0


@imp("epoll_ctl", "fcntl", "ioctl")
def _ok(e):
    return 0


@imp("epoll_wait")
def _epoll_wait(e):
    import sched
    timeout = libc.signed(e.x(3), 32)
    sched.sleep_for(e, 0.05 if timeout < 0 else min(timeout / 1000.0, 0.05))


# ---- calendar time -----------------------------------------------------------
import time as _time

TM_BUF = [0]


def write_tm(e, addr, t):
    e.write(addr, struct.pack("<9i4xqQ", t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon - 1,
                              t.tm_year - 1900, (t.tm_wday + 1) % 7, t.tm_yday - 1, max(t.tm_isdst, 0), 0, 0))
    return addr


def read_tm(e, addr):
    sec, mn, hour, mday, mon, year, _, _, isdst = struct.unpack("<9i", e.read(addr, 36))
    return (year + 1900, mon + 1, mday, hour, mn, sec, 0, 1, isdst)


def static_tm(e):
    if not TM_BUF[0]:
        TM_BUF[0] = e.malloc(64)
    return TM_BUF[0]


@imp("gmtime_r")
def _gmtime_r(e):
    return write_tm(e, e.x(1), _time.gmtime(libc.signed(e.u64(e.x(0)), 64)))


@imp("localtime_r")
def _localtime_r(e):
    return write_tm(e, e.x(1), _time.localtime(libc.signed(e.u64(e.x(0)), 64)))


@imp("gmtime")
def _gmtime(e):
    return write_tm(e, static_tm(e), _time.gmtime(libc.signed(e.u64(e.x(0)), 64)))


@imp("localtime")
def _localtime(e):
    return write_tm(e, static_tm(e), _time.localtime(libc.signed(e.u64(e.x(0)), 64)))


@imp("mktime")
def _mktime(e):
    try:
        return int(_time.mktime(read_tm(e, e.x(0))))
    except (OverflowError, ValueError):
        return 0xFFFFFFFFFFFFFFFF


@imp("difftime")
def _difftime(e):
    e.setd(0, float(libc.signed(e.x(0), 64) - libc.signed(e.x(1), 64)))


@imp("strftime")
def _strftime(e):
    try:
        text = _time.strftime(e.cstr(e.x(2)), _time.struct_time(read_tm(e, e.x(3))))
    except ValueError:
        text = ""
    data = text.encode()[:max(e.x(1) - 1, 0)]
    e.write(e.x(0), data + bytes(1))
    return len(data)


@imp("strerror_r")
def _strerror_r(e):
    import os
    e.write(e.x(1), os.strerror(e.sx(0)).encode()[:max(e.x(2) - 1, 0)] + bytes(1))
    return 0


@imp("statfs")
def _statfs(e):
    e.write(e.x(1), struct.pack("<15q", 0xEF53, 4096, 10_000_000, 5_000_000, 5_000_000, 1_000_000, 500_000,
                                0, 255, 4096, 0, 0, 0, 0, 0))
    return 0

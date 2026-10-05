// bionic libc functions imported by libNOVA.so, implemented on the host.
#include "libc.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <random>
#include <thread>

const char* const PKG = "com.gameloft.android.ANMP.GloftNOHM";

// ---- heap ----------------------------------------------------------------------
namespace {
struct Header { u64 offset; u64 size; };   // sits immediately before the user pointer
Header* header_of(void* p) { return reinterpret_cast<Header*>(static_cast<u8*>(p) - sizeof(Header)); }
}  // namespace

void* g_malloc(size_t n) {
    u8* raw = static_cast<u8*>(malloc(n + sizeof(Header)));
    if (!raw) return nullptr;
    *reinterpret_cast<Header*>(raw) = {sizeof(Header), n};
    return raw + sizeof(Header);
}

void* g_calloc(size_t n, size_t m) {
    void* p = g_malloc(n * m);
    if (p) memset(p, 0, n * m);
    return p;
}

void* g_memalign(size_t align, size_t n) {
    if (align <= 16) return g_malloc(n);
    u8* raw = static_cast<u8*>(malloc(n + align + sizeof(Header)));
    if (!raw) return nullptr;
    u64 user = (reinterpret_cast<u64>(raw) + sizeof(Header) + align - 1) & ~(u64(align) - 1);
    *header_of(reinterpret_cast<void*>(user)) = {user - reinterpret_cast<u64>(raw), n};
    return reinterpret_cast<void*>(user);
}

void g_free(void* p) {
    if (p) free(static_cast<u8*>(p) - header_of(p)->offset);
}

void* g_realloc(void* p, size_t n) {
    if (!p) return g_malloc(n);
    Header* h = header_of(p);
    if (h->offset == sizeof(Header)) {
        u8* raw = static_cast<u8*>(realloc(h, n + sizeof(Header)));
        if (!raw) return nullptr;
        reinterpret_cast<Header*>(raw)->size = n;
        return raw + sizeof(Header);
    }
    void* q = g_malloc(n);
    memcpy(q, p, std::min<size_t>(n, h->size));
    g_free(p);
    return q;
}

char* g_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    return static_cast<char*>(memcpy(g_malloc(n), s, n));
}

int* guest_errno() {
    static thread_local int e = 0;
    return &e;
}

// ---- printf formatting -------------------------------------------------------------
std::string format_guest(const char* fmt, VArgs& a) {
    std::string out;
    char buf[512];
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { out += *p; continue; }
        const char* start = p++;
        std::string spec = "%";
        while (*p && strchr("-+ #0", *p)) spec += *p++;
        if (*p == '*') { spec += std::to_string(static_cast<i32>(a.gp())); p++; }
        else while (*p >= '0' && *p <= '9') spec += *p++;
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') { spec += std::to_string(static_cast<i32>(a.gp())); p++; }
            else while (*p >= '0' && *p <= '9') spec += *p++;
        }
        bool wide = false;      // 64-bit integer argument
        bool is_long = false;
        while (*p && strchr("hlLzjtq", *p)) {
            if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't' || *p == 'q') { wide = true; is_long = true; }
            p++;
        }
        char c = *p;
        if (!c) { out.append(start); break; }
        switch (c) {
        case '%': out += '%'; break;
        case 'd': case 'i': {
            u64 v = a.gp();
            i64 sv = wide ? static_cast<i64>(v) : static_cast<i32>(v);
            snprintf(buf, sizeof buf, (spec + "lld").c_str(), static_cast<long long>(sv));
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            u64 v = a.gp();
            if (!wide) v &= 0xFFFFFFFFull;
            snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), static_cast<unsigned long long>(v));
            out += buf;
            break;
        }
        case 'c':
            snprintf(buf, sizeof buf, (spec + "c").c_str(), static_cast<int>(a.gp() & 0xFF));
            out += buf;
            break;
        case 's': {
            const char* s = reinterpret_cast<const char*>(a.gp());
            if (is_long) { out += "(wide)"; break; }
            if (!s) s = "(null)";
            if (spec == "%") { out += s; break; }
            size_t need = strlen(s) + 64;
            std::string tmp(need + 256, '\0');
            int n = snprintf(tmp.data(), tmp.size(), (spec + "s").c_str(), s);
            if (n > 0) out.append(tmp.data(), std::min<size_t>(n, tmp.size() - 1));
            break;
        }
        case 'p':
            snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(a.gp()));
            out += buf;
            break;
        case 'n': a.gp(); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            snprintf(buf, sizeof buf, (spec + c).c_str(), a.fp());
            out += buf;
            break;
        default:
            out.append(start, p - start + 1);
        }
    }
    return out;
}

// ---- scanf -----------------------------------------------------------------------------
int scan_guest(const char* text, const char* fmt, VArgs& a) {
    const char* s = text;
    int done = 0;
    auto skip_ws = [&] { while (*s && isspace(static_cast<unsigned char>(*s))) s++; };
    for (const char* p = fmt; *p; p++) {
        if (isspace(static_cast<unsigned char>(*p))) { skip_ws(); continue; }
        if (*p != '%') { if (*s != *p) return done; s++; continue; }
        p++;
        bool suppress = false;
        if (*p == '*') { suppress = true; p++; }
        int width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        int size = 4;
        bool lmod = false;
        while (*p && strchr("hlLzjt", *p)) {
            if (*p == 'h') size = size == 2 ? 1 : 2;
            else { size = 8; lmod = true; }
            p++;
        }
        char c = *p;
        if (c == '%') { if (*s != '%') return done; s++; continue; }
        if (c == 'n') { *reinterpret_cast<int*>(a.gp()) = static_cast<int>(s - text); continue; }
        if (c != 'c' && c != '[') skip_ws();
        if (!*s && c != 'c') return done ? done : -1;
        std::string tok;
        auto take = [&](auto pred) {
            while (*s && pred(*s) && (!width || static_cast<int>(tok.size()) < width)) tok += *s++;
        };
        if (strchr("diuxXop", c)) {
            int base = c == 'd' || c == 'u' ? 10 : c == 'o' ? 8 : c == 'i' ? 0 : 16;
            char* end;
            const char* begin = s;
            unsigned long long v = base == 10 && c == 'd' ? static_cast<unsigned long long>(strtoll(s, &end, base))
                                                           : strtoull(s, &end, base);
            if (c == 'i') v = static_cast<unsigned long long>(strtoll(begin, &end, 0));
            if (end == begin) return done;
            if (width && end - begin > width) {
                std::string part(begin, width);
                v = strtoull(part.c_str(), nullptr, base);
                end = const_cast<char*>(begin) + width;
            }
            s = end;
            if (!suppress) { memcpy(reinterpret_cast<void*>(a.gp()), &v, c == 'p' ? 8 : size); done++; }
        } else if (strchr("fFeEgG", c)) {
            char* end;
            double v = strtod(s, &end);
            if (end == s) return done;
            s = end;
            if (!suppress) {
                void* dst = reinterpret_cast<void*>(a.gp());
                if (lmod) memcpy(dst, &v, 8);
                else { float f = static_cast<float>(v); memcpy(dst, &f, 4); }
                done++;
            }
        } else if (c == 's') {
            take([](char ch) { return !isspace(static_cast<unsigned char>(ch)); });
            if (tok.empty()) return done;
            if (!suppress) { memcpy(reinterpret_cast<void*>(a.gp()), tok.c_str(), tok.size() + 1); done++; }
        } else if (c == 'c') {
            int n = width ? width : 1;
            if (static_cast<int>(strlen(s)) < n) return done ? done : -1;
            if (!suppress) { memcpy(reinterpret_cast<void*>(a.gp()), s, n); done++; }
            s += n;
        } else if (c == '[') {
            p++;
            bool neg = false;
            if (*p == '^') { neg = true; p++; }
            std::string set;
            if (*p == ']') set += *p++;
            while (*p && *p != ']') {
                if (p[1] == '-' && p[2] && p[2] != ']') { for (int ch = p[0]; ch <= p[2]; ch++) set += static_cast<char>(ch); p += 3; }
                else set += *p++;
            }
            take([&](char ch) { return (set.find(ch) != std::string::npos) != neg; });
            if (tok.empty()) return done;
            if (!suppress) { memcpy(reinterpret_cast<void*>(a.gp()), tok.c_str(), tok.size() + 1); done++; }
        } else {
            return done;
        }
    }
    return done;
}

// ---- calendar time ------------------------------------------------------------------------
namespace {
struct BionicTm { int sec, min, hour, mday, mon, year, wday, yday, isdst; int pad; i64 gmtoff; const char* zone; };

void to_bionic(const tm& h, BionicTm* b) {
    *b = {h.tm_sec, h.tm_min, h.tm_hour, h.tm_mday, h.tm_mon, h.tm_year, h.tm_wday, h.tm_yday, h.tm_isdst, 0, 0, "UTC"};
}
tm from_bionic(const BionicTm* b) {
    tm h{};
    h.tm_sec = b->sec; h.tm_min = b->min; h.tm_hour = b->hour; h.tm_mday = b->mday; h.tm_mon = b->mon;
    h.tm_year = b->year; h.tm_wday = b->wday; h.tm_yday = b->yday; h.tm_isdst = b->isdst;
    return h;
}
BionicTm* x_gmtime_r(const i64* t, BionicTm* out) { tm h{}; __time64_t tt = *t; _gmtime64_s(&h, &tt); to_bionic(h, out); return out; }
BionicTm* x_localtime_r(const i64* t, BionicTm* out) { tm h{}; __time64_t tt = *t; _localtime64_s(&h, &tt); to_bionic(h, out); return out; }
thread_local BionicTm tl_tm;

struct Timespec { i64 sec, nsec; };
int x_clock_gettime(int clk, Timespec* ts) {
    using namespace std::chrono;
    i64 ns = clk == 0 ? duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count()
                      : duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    ts->sec = ns / 1000000000; ts->nsec = ns % 1000000000;
    return 0;
}

// ---- qsort / bsearch with a guest comparator ----------------------------------------------
void x_qsort(Thread& t, u8* base, size_t n, size_t size, u64 cmp) {
    if (n < 2) return;
    std::vector<u8*> idx(n);
    std::vector<u8> copy(base, base + n * size);
    for (size_t i = 0; i < n; i++) idx[i] = copy.data() + i * size;
    std::stable_sort(idx.begin(), idx.end(), [&](u8* a, u8* b) {
        return static_cast<i32>(t.call(cmp, {reinterpret_cast<u64>(a), reinterpret_cast<u64>(b)})) < 0;
    });
    for (size_t i = 0; i < n; i++) memcpy(base + i * size, idx[i], size);
}

void* x_bsearch(Thread& t, const void* key, u8* base, size_t n, size_t size, u64 cmp) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        i32 r = static_cast<i32>(t.call(cmp, {reinterpret_cast<u64>(key), reinterpret_cast<u64>(base + mid * size)}));
        if (r == 0) return base + mid * size;
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return nullptr;
}

// ---- wide characters (32-bit wchar_t, UTF-8 multibyte) ----------------------------------------
using wc = u32;
size_t x_wcslen(const wc* s) { size_t n = 0; while (s[n]) n++; return n; }
int x_wcscmp(const wc* a, const wc* b) { while (*a && *a == *b) a++, b++; return *a < *b ? -1 : *a > *b; }

size_t x_mbrtowc(wc* out, const u8* s, size_t n, void*) {
    if (!s) return 0;
    if (!n) return static_cast<size_t>(-2);
    u32 c = s[0];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len) { *guest_errno() = 84; return static_cast<size_t>(-1); }
    if (static_cast<size_t>(len) > n) return static_cast<size_t>(-2);
    if (len > 1) { c &= 0xFF >> (len + 1); for (int i = 1; i < len; i++) c = (c << 6) | (s[i] & 0x3F); }
    if (out) *out = c;
    return c ? len : 0;
}
size_t x_wcrtomb(u8* out, wc c, void*) {
    u8 tmp[4];
    if (!out) out = tmp;
    if (c < 0x80) { out[0] = static_cast<u8>(c); return 1; }
    if (c < 0x800) { out[0] = 0xC0 | (c >> 6); out[1] = 0x80 | (c & 0x3F); return 2; }
    if (c < 0x10000) { out[0] = 0xE0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3F); out[2] = 0x80 | (c & 0x3F); return 3; }
    out[0] = 0xF0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 0x3F); out[2] = 0x80 | ((c >> 6) & 0x3F); out[3] = 0x80 | (c & 0x3F);
    return 4;
}
size_t x_mbsnrtowcs(wc* dst, const u8** src, size_t nms, size_t len, void*) {
    const u8* s = *src;
    size_t n = 0;
    while (nms && (!dst || n < len)) {
        wc c;
        size_t r = x_mbrtowc(&c, s, nms, nullptr);
        if (r == static_cast<size_t>(-1)) return r;
        if (r == static_cast<size_t>(-2)) break;
        if (dst) dst[n] = c;
        if (r == 0) { *src = nullptr; return n; }
        s += r; nms -= r; n++;
    }
    if (dst) *src = s;
    return n;
}
size_t x_wcsnrtombs(u8* dst, const wc** src, size_t nwc, size_t len, void*) {
    const wc* s = *src;
    size_t n = 0;
    while (nwc) {
        u8 tmp[4];
        size_t r = x_wcrtomb(tmp, *s, nullptr);
        if (dst && n + r > len) break;
        if (dst) memcpy(dst + n, tmp, r);
        if (!*s) { *src = nullptr; return n; }
        n += r; s++; nwc--;
    }
    if (dst) *src = s;
    return n;
}

const char* const WCTYPES[] = {"alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
                               "space", "upper", "xdigit"};
int a_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int a_isdigit(int c) { return c >= '0' && c <= '9'; }
int a_isalnum(int c) { return a_isalpha(c) || a_isdigit(c); }
int a_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int a_isupper(int c) { return c >= 'A' && c <= 'Z'; }
int a_islower(int c) { return c >= 'a' && c <= 'z'; }
int a_iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int a_isprint(int c) { return c >= 32 && c < 127; }
int a_isgraph(int c) { return c > 32 && c < 127; }
int a_ispunct(int c) { return a_isgraph(c) && !a_isalnum(c); }
int a_isxdigit(int c) { return a_isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int a_isblank(int c) { return c == ' ' || c == '\t'; }
int a_tolower(int c) { return a_isupper(c) ? c + 32 : c; }
int a_toupper(int c) { return a_islower(c) ? c - 32 : c; }
int x_iswctype(int c, u64 type) {
    switch (type) {
    case 1: return a_isalnum(c); case 2: return a_isalpha(c); case 3: return a_isblank(c); case 4: return a_iscntrl(c);
    case 5: return a_isdigit(c); case 6: return a_isgraph(c); case 7: return a_islower(c); case 8: return a_isprint(c);
    case 9: return a_ispunct(c); case 10: return a_isspace(c); case 11: return a_isupper(c); case 12: return a_isxdigit(c);
    }
    return 0;
}

struct Lconv { const char* p[10]; char c[14]; };
Lconv g_lconv = {{".", "", "", "", "", "", "", "", "", ""}, {127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127}};
}  // namespace

// ---- registration ---------------------------------------------------------------------------
void register_libc() {
    // heap
    IMPORT("malloc", g_malloc);
    IMPORT("calloc", g_calloc);
    IMPORT("realloc", g_realloc);
    IMPORT("memalign", g_memalign);
    IMPORT("free", g_free);
    IMPORT("mmap", [](void*, size_t n, int, int, int, i64) -> void* {
        void* p = VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return p ? p : reinterpret_cast<void*>(~0ull);
    });
    IMPORT("munmap", [](void* p, size_t) -> int { VirtualFree(p, 0, MEM_RELEASE); return 0; });

    // memory and strings: the host CRT works directly on guest pointers
    IMPORT("memcpy", [](void* d, const void* s, size_t n) { return memcpy(d, s, n); });
    IMPORT("memmove", [](void* d, const void* s, size_t n) { return memmove(d, s, n); });
    IMPORT("memset", [](void* d, int c, size_t n) { return memset(d, c, n); });
    IMPORT("memcmp", [](const void* a, const void* b, size_t n) { return memcmp(a, b, n); });
    IMPORT("memchr", [](const void* a, int c, size_t n) { return const_cast<void*>(memchr(a, c, n)); });
    IMPORT("strlen", [](const char* s) { return strlen(s); });
    IMPORT("strnlen", [](const char* s, size_t n) { return strnlen(s, n); });
    IMPORT("strcmp", [](const char* a, const char* b) { return strcmp(a, b); });
    IMPORT("strcoll", [](const char* a, const char* b) { return strcmp(a, b); });
    IMPORT("strcoll_l", [](const char* a, const char* b) { return strcmp(a, b); });
    IMPORT("strncmp", [](const char* a, const char* b, size_t n) { return strncmp(a, b, n); });
    IMPORT("strcasecmp", [](const char* a, const char* b) { return _stricmp(a, b); });
    IMPORT("strncasecmp", [](const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); });
    IMPORT("strcpy", [](char* d, const char* s) { return strcpy(d, s); });
    IMPORT("strncpy", [](char* d, const char* s, size_t n) { return strncpy(d, s, n); });
    IMPORT("strcat", [](char* d, const char* s) { return strcat(d, s); });
    IMPORT("strncat", [](char* d, const char* s, size_t n) { return strncat(d, s, n); });
    IMPORT("strchr", [](const char* s, int c) { return const_cast<char*>(strchr(s, c)); });
    IMPORT("strrchr", [](const char* s, int c) { return const_cast<char*>(strrchr(s, c)); });
    IMPORT("strstr", [](const char* a, const char* b) { return const_cast<char*>(strstr(a, b)); });
    IMPORT("strspn", [](const char* a, const char* b) { return strspn(a, b); });
    IMPORT("strcspn", [](const char* a, const char* b) { return strcspn(a, b); });
    IMPORT("strpbrk", [](const char* a, const char* b) { return const_cast<char*>(strpbrk(a, b)); });
    IMPORT("strtok", [](char* s, const char* d) { static thread_local char* save; return strtok_s(s, d, &save); });
    IMPORT("strtok_r", [](char* s, const char* d, char** save) { return strtok_s(s, d, save); });
    IMPORT("strdup", g_strdup);
    IMPORT("strerror", [](int e) { return strerror(e); });
    IMPORT("strerror_r", [](int e, char* buf, size_t n) -> int { strncpy(buf, strerror(e), n); if (n) buf[n - 1] = 0; return 0; });
    auto xfrm = [](char* d, const char* s, size_t n) -> size_t { size_t len = strlen(s); if (n) { strncpy(d, s, n); } return len; };
    IMPORT("strxfrm", xfrm);
    IMPORT("strxfrm_l", xfrm);

    // numbers
    IMPORT("atoi", [](const char* s) { return atoi(s); });
    IMPORT("atol", [](const char* s) { return atoll(s); });
    IMPORT("atof", [](const char* s) { return atof(s); });
    IMPORT("strtod", [](const char* s, char** e) { return strtod(s, e); });
    IMPORT("strtof", [](const char* s, char** e) { return strtof(s, e); });
    IMPORT("strtold", [](const char* s, char** e) { return strtod(s, e); });
    IMPORT("strtold_l", [](const char* s, char** e) { return strtod(s, e); });
    auto to_ll = [](const char* s, char** e, int b) { return strtoll(s, e, b); };
    auto to_ull = [](const char* s, char** e, int b) { return strtoull(s, e, b); };
    IMPORT("strtol", to_ll); IMPORT("strtoll", to_ll); IMPORT("strtoll_l", to_ll);
    IMPORT("strtoul", to_ull); IMPORT("strtoull", to_ull); IMPORT("strtoull_l", to_ull);
    IMPORT("rand", []() -> int { static thread_local std::minstd_rand r{std::random_device{}()}; return static_cast<int>(r() & 0x7FFFFFFF); });
    IMPORT("srand", [](unsigned) {});
    IMPORT("isnan", [](double v) -> int { return std::isnan(v); });

    // ctype (ASCII, as in the C locale)
    struct { const char* name; int (*fn)(int); } ct[] = {
        {"isalnum", a_isalnum}, {"isalpha", a_isalpha}, {"iscntrl", a_iscntrl}, {"islower", a_islower},
        {"ispunct", a_ispunct}, {"isspace", a_isspace}, {"isupper", a_isupper}, {"isxdigit", a_isxdigit},
        {"tolower", a_tolower}, {"toupper", a_toupper}, {"isdigit_l", a_isdigit}, {"islower_l", a_islower},
        {"isupper_l", a_isupper}, {"isxdigit_l", a_isxdigit}, {"tolower_l", a_tolower}, {"toupper_l", a_toupper},
        {"iswalpha_l", a_isalpha}, {"iswblank_l", a_isblank}, {"iswcntrl_l", a_iscntrl}, {"iswdigit_l", a_isdigit},
        {"iswlower_l", a_islower}, {"iswprint_l", a_isprint}, {"iswpunct_l", a_ispunct}, {"iswspace_l", a_isspace},
        {"iswupper_l", a_isupper}, {"iswxdigit_l", a_isxdigit}, {"towlower", a_tolower}, {"towlower_l", a_tolower},
        {"towupper", a_toupper}, {"towupper_l", a_toupper}};
    for (auto& c : ct) guest::reg(c.name, wrap(c.fn));
    IMPORT("iswctype", x_iswctype);
    IMPORT("wctype", [](const char* name) -> u64 {
        for (int i = 0; i < 12; i++) if (!strcmp(name, WCTYPES[i])) return i + 1;
        return 0;
    });
    guest::reg_data("_ctype_", 8, [](u8* p) {
        static u8 table[257];
        for (int c = 0; c < 128; c++) {
            u8 f = 0;
            if (a_isupper(c)) f |= 1; if (a_islower(c)) f |= 2; if (a_isdigit(c)) f |= 4; if (a_isspace(c)) f |= 8;
            if (a_ispunct(c)) f |= 16; if (a_iscntrl(c)) f |= 32; if (a_isxdigit(c) && !a_isdigit(c)) f |= 64;
            if (c == ' ') f |= 128;
            table[c + 1] = f;
        }
        const u8* t = table;
        memcpy(p, &t, 8);
    });

    // maths
#define MATH1(n) IMPORT(#n, [](double v) { return n(v); }); IMPORT(#n "f", [](float v) { return n##f(v); })
#define MATH2(n) IMPORT(#n, [](double a, double b) { return n(a, b); }); IMPORT(#n "f", [](float a, float b) { return n##f(a, b); })
    MATH1(sin); MATH1(cos); MATH1(tan); MATH1(asin); MATH1(acos); MATH1(atan); MATH1(exp); MATH1(exp2); MATH1(log);
    MATH1(log10); MATH1(sqrt); MATH1(sinh); MATH1(cosh); MATH1(tanh);
    MATH2(atan2); MATH2(pow); MATH2(fmod);
    IMPORT("ldexp", [](double v, int e) { return ldexp(v, e); });
    IMPORT("ldexpf", [](float v, int e) { return ldexpf(v, e); });
    IMPORT("frexp", [](double v, int* e) { return frexp(v, e); });
    IMPORT("modf", [](double v, double* i) { return modf(v, i); });
    IMPORT("modff", [](float v, float* i) { return modff(v, i); });

    // time
    IMPORT("clock_gettime", x_clock_gettime);
    IMPORT("gettimeofday", [](Timespec* tv, void*) -> int {
        Timespec ts; x_clock_gettime(0, &ts);
        tv->sec = ts.sec; tv->nsec = ts.nsec / 1000;
        return 0;
    });
    IMPORT("time", [](i64* out) -> i64 { i64 t = _time64(nullptr); if (out) *out = t; return t; });
    IMPORT("clock", []() -> i64 { Timespec ts; x_clock_gettime(1, &ts); return ts.sec * 1000000 + ts.nsec / 1000; });
    IMPORT("gmtime_r", x_gmtime_r);
    IMPORT("localtime_r", x_localtime_r);
    IMPORT("gmtime", [](const i64* t) { return x_gmtime_r(t, &tl_tm); });
    IMPORT("localtime", [](const i64* t) { return x_localtime_r(t, &tl_tm); });
    IMPORT("mktime", [](BionicTm* b) -> i64 { tm h = from_bionic(b); i64 r = _mktime64(&h); to_bionic(h, b); return r; });
    IMPORT("difftime", [](i64 a, i64 b) { return static_cast<double>(a - b); });
    auto ftime = [](char* out, size_t n, const char* fmt, const BionicTm* b) -> size_t {
        tm h = from_bionic(b);
        return strftime(out, n, fmt, &h);
    };
    IMPORT("strftime", ftime);
    IMPORT("strftime_l", ftime);
    IMPORT("usleep", [](u32 us) -> int { std::this_thread::sleep_for(std::chrono::microseconds(us)); return 0; });
    IMPORT("nanosleep", [](const Timespec* ts, void*) -> int {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ts->sec * 1000000000 + ts->nsec));
        return 0;
    });
    IMPORT("sched_yield", []() -> int { std::this_thread::yield(); return 0; });
    guest::reg_data("timezone", 8);

    // process and environment
    IMPORT("__errno", guest_errno);
    IMPORT("getenv", [](const char*) -> char* { return nullptr; });
    IMPORT("getpid", []() -> int { return 4242; });
    auto uid = []() -> int { return 10123; };
    IMPORT("getuid", uid); IMPORT("geteuid", uid); IMPORT("getgid", uid); IMPORT("getegid", uid);
    IMPORT("getpwuid", [](int) -> void* { return nullptr; });
    IMPORT("sysconf", [](int n) -> i64 {
        switch (n) {
        case 39: case 40: return 4096;
        case 96: case 97: return std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
        case 98: return 1 << 20;
        }
        return -1;
    });
    IMPORT("getpagesize", []() -> int { return 4096; });
    IMPORT("syscall", [](i64) -> i64 { return 0; });
    IMPORT("system", [](const char*) -> int { return -1; });
    IMPORT("abort", [](Thread& t) { fatal("** guest called abort()"); });
    IMPORT("__stack_chk_fail", [](Thread& t) {
        fatal("** guest stack check failed (x8=%llx x9=%llx x21=%llx x26=%llx tpidr=%llx sp=%llx)", t.x(8), t.x(9), t.x(21), t.x(26), t.tpidr, t.sp());
    });
    IMPORT("exit", [](Thread& t, int code) { logf("** guest called exit(%d)", code); fflush(stdout); TerminateProcess(GetCurrentProcess(), code); });
    IMPORT("android_set_abort_message", [](const char* m) { logf("[abort message] %s", m); });
    IMPORT("qsort", x_qsort);
    IMPORT("bsearch", x_bsearch);
    for (const char* n : {"__cxa_atexit", "__cxa_finalize", "sigaction", "signal", "openlog", "closelog", "syslog",
                          "umask", "tcgetattr", "tcsetattr"})
        guest::reg(n, [](Thread& t) { t.setx(0, 0); });
    IMPORT("dl_iterate_phdr", [](Thread& t, u64 cb, u64 data) -> int {
        struct { u64 addr; const char* name; u64 phdr; u16 phnum; u8 pad[38]; } info{};
        const u8* f = guest::image_file().data();
        info.addr = guest::image_base();
        info.name = "libNOVA.so";
        info.phdr = guest::image_base() + *reinterpret_cast<const u64*>(f + 0x20);
        info.phnum = *reinterpret_cast<const u16*>(f + 0x38);
        return static_cast<int>(t.call(cb, {reinterpret_cast<u64>(&info), sizeof info, data}));
    });

    // setjmp / longjmp: callee-saved state lives in the guest's jmp_buf (256 bytes)
    guest::reg("setjmp", [](Thread& t) {
        u64* buf = reinterpret_cast<u64*>(t.x(0));
        for (int i = 0; i < 12; i++) buf[i] = t.x(19 + i);
        buf[12] = t.sp();
        for (int i = 0; i < 8; i++) { double v = t.d(8 + i); memcpy(&buf[13 + i], &v, 8); }
        t.setx(0, 0);
    });
    guest::reg("longjmp", [](Thread& t) {
        const u64* buf = reinterpret_cast<const u64*>(t.x(0));
        u64 val = t.x(1) ? t.x(1) : 1;
        for (int i = 0; i < 12; i++) t.setx(19 + i, buf[i]);
        t.set_sp(buf[12]);
        for (int i = 0; i < 8; i++) { double v; memcpy(&v, &buf[13 + i], 8); t.setd(8 + i, v); }
        t.setx(0, val);          // the stub's `ret` now returns to setjmp's caller
    });

    // locale and wide characters
    auto locale_token = []() -> void* { static int token; return &token; };
    guest::reg("newlocale", [=](Thread& t) { t.setx(0, reinterpret_cast<u64>(locale_token())); });
    guest::reg("uselocale", [=](Thread& t) { t.setx(0, reinterpret_cast<u64>(locale_token())); });
    guest::reg("freelocale", [](Thread& t) { t.setx(0, 0); });
    IMPORT("setlocale", [](int, const char*) -> const char* { return "C"; });
    IMPORT("localeconv", []() -> void* { return &g_lconv; });
    IMPORT("__ctype_get_mb_cur_max", []() -> size_t { return 4; });
    IMPORT("btowc", [](int c) -> u32 { return c >= 0 && c < 128 ? c : ~0u; });
    IMPORT("wctob", [](u32 c) -> int { return c < 128 ? static_cast<int>(c) : -1; });
    IMPORT("mbrtowc", x_mbrtowc);
    IMPORT("mbrlen", [](const u8* s, size_t n, void* st) { return x_mbrtowc(nullptr, s, n, st); });
    IMPORT("mbtowc", [](wc* out, const u8* s, size_t n) -> int { return s ? static_cast<int>(x_mbrtowc(out, s, n, nullptr)) : 0; });
    IMPORT("wcrtomb", x_wcrtomb);
    IMPORT("mbsnrtowcs", x_mbsnrtowcs);
    IMPORT("mbsrtowcs", [](wc* d, const u8** s, size_t len, void* st) { return x_mbsnrtowcs(d, s, ~size_t(0) >> 1, len, st); });
    IMPORT("wcsnrtombs", x_wcsnrtombs);
    IMPORT("wcslen", x_wcslen);
    IMPORT("wcscmp", x_wcscmp);
    IMPORT("wcscoll", x_wcscmp);
    IMPORT("wcscoll_l", x_wcscmp);
    IMPORT("wcscpy", [](wc* d, const wc* s) { memcpy(d, s, 4 * (x_wcslen(s) + 1)); return d; });
    IMPORT("wmemcpy", [](wc* d, const wc* s, size_t n) { memcpy(d, s, 4 * n); return d; });
    IMPORT("wmemmove", [](wc* d, const wc* s, size_t n) { memmove(d, s, 4 * n); return d; });
    IMPORT("wmemset", [](wc* d, wc c, size_t n) { for (size_t i = 0; i < n; i++) d[i] = c; return d; });
    IMPORT("wmemcmp", [](const wc* a, const wc* b, size_t n) -> int {
        for (size_t i = 0; i < n; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    });
    IMPORT("wmemchr", [](const wc* s, wc c, size_t n) -> const wc* {
        for (size_t i = 0; i < n; i++) if (s[i] == c) return s + i;
        return nullptr;
    });
    auto wxfrm = [](wc* d, const wc* s, size_t n) -> size_t {
        size_t len = x_wcslen(s);
        for (size_t i = 0; i < n && i <= len; i++) d[i] = s[i];
        return len;
    };
    IMPORT("wcsxfrm", wxfrm);
    IMPORT("wcsxfrm_l", wxfrm);
}

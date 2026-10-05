// Files: path mapping into the sandbox, FILE* streams, raw descriptors,
// directories and the printf/scanf entry points.
#include "libc.h"

#include <windows.h>

#include <direct.h>
#include <io.h>
#include <sys/stat.h>

#include <filesystem>
#include <map>
#include <mutex>
#include <random>

namespace fs = std::filesystem;

// ---- path mapping -----------------------------------------------------------------
namespace {
std::mutex g_cwd_mutex;
std::string g_cwd = "/";

const std::map<std::string, std::string>& fake_files() {
    static const std::map<std::string, std::string> files = {
        {"/proc/cpuinfo",
         "Processor\t: AArch64 Processor rev 4 (aarch64)\nprocessor\t: 0\nBogoMIPS\t: 38.40\n"
         "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\nCPU implementer\t: 0x51\n"
         "CPU architecture: 8\nCPU variant\t: 0xa\nCPU part\t: 0x801\nCPU revision\t: 4\n\n"
         "Hardware\t: Qualcomm Technologies, Inc MSM8998\n"},
        {"/proc/meminfo", "MemTotal:        5856780 kB\nMemFree:         2856780 kB\nMemAvailable:    3856780 kB\n"},
        {"/sys/devices/system/cpu/present", "0-3\n"},
        {"/sys/devices/system/cpu/possible", "0-3\n"},
    };
    return files;
}

std::string find_apk() {
    for (auto& e : fs::directory_iterator(project_root()))
        if (e.path().extension() == ".apk") return e.path().string();
    return "";
}
}  // namespace

std::string guest_cwd() { std::lock_guard lk(g_cwd_mutex); return g_cwd; }

std::string host_path(const char* guest_path) {
    std::string g = guest_path ? guest_path : "";
    for (auto& c : g) if (c == '\\') c = '/';
    if (g.empty()) return "";
    if (g[0] != '/') {
        std::string cwd = guest_cwd();
        if (cwd.back() != '/') cwd += '/';
        while (g.rfind("./", 0) == 0) g = g.substr(2);
        g = cwd + g;
    }
    std::string pkg = PKG;
    std::string root = project_root() + "/work";
    if (g == "/data/app/" + pkg + "-1/base.apk") return find_apk();
    if (g == "/data/app/" + pkg + "-1/lib/arm64/libNOVA.so") return root + "/lib/arm64-v8a/libNOVA.so";
    const std::pair<std::string, std::string> maps[] = {
        {"/data/data/" + pkg, "/fs/data"}, {"/data/user/0/" + pkg, "/fs/data"},
        {"/sdcard/Android/data/" + pkg, "/fs/sdcard_app"}, {"/storage/emulated/0/Android/data/" + pkg, "/fs/sdcard_app"},
        {"/sdcard", "/fs/sdcard"}, {"/storage/emulated/0", "/fs/sdcard"}};
    for (auto& [prefix, sub] : maps)
        if (g.rfind(prefix, 0) == 0 && (g.size() == prefix.size() || g[prefix.size()] == '/'))
            return root + sub + g.substr(prefix.size());
    return "";
}

bool set_guest_cwd(const char* path) {
    std::string hp = host_path(path);
    if (hp.empty() || !fs::is_directory(hp)) return false;
    std::lock_guard lk(g_cwd_mutex);
    g_cwd = path;
    return true;
}

// ---- streams ------------------------------------------------------------------------
namespace {
struct GFile {
    FILE* f = nullptr;
    int std_stream = 0;                 // 1 = stdout, 2 = stderr
    std::string mem;                    // contents of a fake /proc file or /dev/urandom marker
    size_t mem_pos = 0;
    bool is_mem = false, is_random = false, eof = false;
    u64 cookie = 0, readfn = 0, writefn = 0, seekfn = 0, closefn = 0;   // funopen
    bool is_fun() const { return readfn || writefn || seekfn || closefn; }
};

u8* g_sF = nullptr;                     // bionic's __sF[3], 152 bytes each
GFile g_std[3];

GFile* gf(void* p) {
    u8* b = static_cast<u8*>(p);
    if (g_sF && b >= g_sF && b < g_sF + 3 * 152) return &g_std[(b - g_sF) / 152];
    return static_cast<GFile*>(p);
}

GFile* open_guest(const char* path, const char* mode) {
    std::string p = path ? path : "";
    auto fake = fake_files().find(p);
    if (fake != fake_files().end()) {
        auto* g = new GFile;
        g->is_mem = true;
        g->mem = fake->second;
        return g;
    }
    if (p == "/dev/urandom" || p == "/dev/random") {
        auto* g = new GFile;
        g->is_random = true;
        return g;
    }
    std::string hp = host_path(path);
    if (hp.empty()) return nullptr;
    // Rebuild the mode from its meaning: the Windows CRT aborts the process on
    // mode strings it does not recognise (bionic accepts extras such as "e").
    std::string in = mode ? mode : "r";
    std::string m(1, in.find('w') != std::string::npos ? 'w' : in.find('a') != std::string::npos ? 'a' : 'r');
    if (in.find('+') != std::string::npos) m += '+';
    m += 'b';
    FILE* f = fopen(hp.c_str(), m.c_str());
    if (!f) return nullptr;
    auto* g = new GFile;
    g->f = f;
    return g;
}

size_t g_read(Thread& t, GFile* g, void* buf, size_t n) {
    if (g->is_random) {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        for (size_t i = 0; i < n; i++) static_cast<u8*>(buf)[i] = static_cast<u8>(rng());
        return n;
    }
    if (g->is_mem) {
        size_t k = std::min(n, g->mem.size() - g->mem_pos);
        memcpy(buf, g->mem.data() + g->mem_pos, k);
        g->mem_pos += k;
        if (k < n) g->eof = true;
        return k;
    }
    if (g->is_fun()) {
        size_t got = 0;
        while (got < n && g->readfn) {
            i32 r = static_cast<i32>(t.call(g->readfn, {g->cookie, reinterpret_cast<u64>(buf) + got,
                                                         std::min<u64>(n - got, 1 << 30)}));
            if (r <= 0) { g->eof = true; break; }
            got += r;
        }
        return got;
    }
    if (!g->f) return 0;
    size_t k = fread(buf, 1, n, g->f);
    if (k < n) g->eof = true;
    return k;
}

size_t g_write(Thread& t, GFile* g, const void* buf, size_t n) {
    if (g->std_stream) {
        std::string s(static_cast<const char*>(buf), n);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) logf("[%s] %s", g->std_stream == 1 ? "stdout" : "stderr", s.c_str());
        return n;
    }
    if (g->is_fun()) return g->writefn ? static_cast<i32>(t.call(g->writefn, {g->cookie, reinterpret_cast<u64>(buf), n})) : 0;
    return g->f ? fwrite(buf, 1, n, g->f) : 0;
}

i64 g_seek(Thread& t, GFile* g, i64 off, int whence) {
    g->eof = false;
    if (g->is_mem) {
        i64 base = whence == 0 ? 0 : whence == 1 ? static_cast<i64>(g->mem_pos) : static_cast<i64>(g->mem.size());
        g->mem_pos = static_cast<size_t>(std::clamp<i64>(base + off, 0, g->mem.size()));
        return static_cast<i64>(g->mem_pos);
    }
    if (g->is_fun()) return g->seekfn ? static_cast<i64>(t.call(g->seekfn, {g->cookie, static_cast<u64>(off), static_cast<u64>(whence)})) : -1;
    if (!g->f) return -1;
    if (_fseeki64(g->f, off, whence) != 0) return -1;
    return _ftelli64(g->f);
}

i64 g_tell(Thread& t, GFile* g) {
    if (g->is_mem) return static_cast<i64>(g->mem_pos);
    if (g->is_fun()) return g_seek(t, g, 0, 1);
    return g->f ? _ftelli64(g->f) : -1;
}

int g_close(Thread& t, GFile* g) {
    if (g->std_stream) return 0;
    if (g->is_fun() && g->closefn) t.call(g->closefn, {g->cookie});
    if (g->f) fclose(g->f);
    delete g;
    return 0;
}

int g_getc(Thread& t, GFile* g) {
    u8 c;
    return g_read(t, g, &c, 1) == 1 ? c : -1;
}

// ---- descriptors ------------------------------------------------------------------------
std::mutex g_fd_mutex;
std::map<int, GFile*> g_fds;
int g_next_fd = 100;

int new_fd(GFile* g) {
    std::lock_guard lk(g_fd_mutex);
    g_fds[g_next_fd] = g;
    return g_next_fd++;
}
GFile* fd_file(int fd) {
    std::lock_guard lk(g_fd_mutex);
    auto it = g_fds.find(fd);
    return it == g_fds.end() ? nullptr : it->second;
}

struct BionicStat {      // struct stat, arm64
    u64 dev, ino; u32 mode, nlink, uid, gid; u64 rdev, pad1; i64 size; i32 blksize, pad2; i64 blocks;
    i64 atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns; u32 unused[2];
};

int fill_stat(const std::string& hp, BionicStat* st) {
    struct _stat64 s;
    if (hp.empty() || _stat64(hp.c_str(), &s) != 0) { *guest_errno() = 2; return -1; }
    memset(st, 0, sizeof *st);
    st->mode = (s.st_mode & _S_IFDIR) ? 0040755 : 0100644;
    st->nlink = 1;
    st->size = s.st_size;
    st->blksize = 4096;
    st->blocks = (s.st_size + 511) / 512;
    st->atime = s.st_atime; st->mtime = s.st_mtime; st->ctime = s.st_ctime;
    return 0;
}

struct BionicDirent { u64 ino; i64 off; u16 reclen; u8 type; char name[256]; };
struct GDir { fs::directory_iterator it, end; BionicDirent ent; };
}  // namespace

void register_stdio() {
    static_assert(sizeof(BionicStat) == 128);
    g_std[1].std_stream = 1;
    g_std[2].std_stream = 2;
    guest::reg_data("__sF", 3 * 152, [](u8* p) { g_sF = p; });
    for (const char* d : {"/work/fs/data/files", "/work/fs/data/cache", "/work/fs/sdcard", "/work/fs/sdcard_app/files"})
        fs::create_directories(project_root() + d);

    IMPORT("fopen", [](const char* path, const char* mode) -> void* {
        GFile* g = open_guest(path, mode);
        if (!g) { *guest_errno() = 2; log_once(std::string("fopen:") + (path ? path : ""), "[fopen] %s (%s) -> not found", path, mode); }
        return g;
    });
    IMPORT("funopen", [](u64 cookie, u64 rd, u64 wr, u64 sk, u64 cl) -> void* {
        auto* g = new GFile;
        g->cookie = cookie; g->readfn = rd; g->writefn = wr; g->seekfn = sk; g->closefn = cl;
        return g;
    });
    IMPORT("fdopen", [](int fd, const char*) -> void* { return fd_file(fd); });
    IMPORT("fclose", [](Thread& t, void* f) -> int { return f ? g_close(t, gf(f)) : -1; });
    IMPORT("fread", [](Thread& t, void* buf, size_t size, size_t n, void* f) -> size_t {
        if (!size || !n || !f) return 0;
        return g_read(t, gf(f), buf, size * n) / size;
    });
    IMPORT("fwrite", [](Thread& t, const void* buf, size_t size, size_t n, void* f) -> size_t {
        if (!size || !n || !f) return 0;
        return g_write(t, gf(f), buf, size * n) / size;
    });
    auto seek = [](Thread& t, void* f, i64 off, int whence) -> int { return g_seek(t, gf(f), off, whence) < 0 ? -1 : 0; };
    IMPORT("fseek", seek); IMPORT("fseeko", seek);
    auto tell = [](Thread& t, void* f) -> i64 { return g_tell(t, gf(f)); };
    IMPORT("ftell", tell); IMPORT("ftello", tell);
    IMPORT("rewind", [](Thread& t, void* f) { g_seek(t, gf(f), 0, 0); });
    IMPORT("feof", [](void* f) -> int { return gf(f)->eof; });
    IMPORT("ferror", [](void*) -> int { return 0; });
    IMPORT("clearerr", [](void* f) { gf(f)->eof = false; });
    IMPORT("fflush", [](void* f) -> int { if (f && gf(f)->f) fflush(gf(f)->f); return 0; });
    IMPORT("fileno", [](void* f) -> int { return gf(f)->std_stream ? gf(f)->std_stream : new_fd(gf(f)); });
    IMPORT("setvbuf", [](void*, char*, int, size_t) -> int { return 0; });
    auto getc_ = [](Thread& t, void* f) -> int { return g_getc(t, gf(f)); };
    IMPORT("getc", getc_);
    IMPORT("ungetc", [](Thread& t, int c, void* f) -> int { g_seek(t, gf(f), -1, 1); return c; });
    IMPORT("fgets", [](Thread& t, char* buf, int n, void* f) -> char* {
        int i = 0;
        while (i < n - 1) {
            int c = g_getc(t, gf(f));
            if (c < 0) break;
            buf[i++] = static_cast<char>(c);
            if (c == '\n') break;
        }
        if (!i) return nullptr;
        buf[i] = 0;
        return buf;
    });
    auto putc_ = [](Thread& t, int c, void* f) -> int { char ch = static_cast<char>(c); g_write(t, gf(f), &ch, 1); return c & 0xFF; };
    IMPORT("fputc", putc_); IMPORT("putc", putc_);
    IMPORT("putchar", [](int c) -> int { return c; });
    IMPORT("fputs", [](Thread& t, const char* s, void* f) -> int { g_write(t, gf(f), s, strlen(s)); return 1; });
    IMPORT("puts", [](const char* s) -> int { logf("[stdout] %s", s); return 1; });
    IMPORT("perror", [](const char* s) { logf("[perror] %s", s); });
    IMPORT("tmpfile", []() -> void* { return nullptr; });
    IMPORT("freopen", [](const char*, const char*, void* f) -> void* { return f; });

    // printf family
    guest::reg("printf", [](Thread& t) {
        RegArgs a(t, 1);
        std::string s = format_guest(reinterpret_cast<const char*>(t.x(0)), a);
        g_write(t, &g_std[1], s.data(), s.size());
        t.setx(0, s.size());
    });
    guest::reg("fprintf", [](Thread& t) {
        RegArgs a(t, 2);
        std::string s = format_guest(reinterpret_cast<const char*>(t.x(1)), a);
        g_write(t, gf(reinterpret_cast<void*>(t.x(0))), s.data(), s.size());
        t.setx(0, s.size());
    });
    guest::reg("vfprintf", [](Thread& t) {
        VaList a(t.x(2));
        std::string s = format_guest(reinterpret_cast<const char*>(t.x(1)), a);
        g_write(t, gf(reinterpret_cast<void*>(t.x(0))), s.data(), s.size());
        t.setx(0, s.size());
    });
    auto emit = [](Thread& t, char* dst, size_t cap, const std::string& s) {
        if (dst && cap) { size_t k = std::min(s.size(), cap - 1); memcpy(dst, s.data(), k); dst[k] = 0; }
        t.setx(0, s.size());
    };
    guest::reg("snprintf", [emit](Thread& t) {
        RegArgs a(t, 3);
        emit(t, reinterpret_cast<char*>(t.x(0)), t.x(1), format_guest(reinterpret_cast<const char*>(t.x(2)), a));
    });
    guest::reg("sprintf", [emit](Thread& t) {
        RegArgs a(t, 2);
        emit(t, reinterpret_cast<char*>(t.x(0)), ~size_t(0), format_guest(reinterpret_cast<const char*>(t.x(1)), a));
    });
    guest::reg("vsnprintf", [emit](Thread& t) {
        VaList a(t.x(3));
        emit(t, reinterpret_cast<char*>(t.x(0)), t.x(1), format_guest(reinterpret_cast<const char*>(t.x(2)), a));
    });
    guest::reg("vsprintf", [emit](Thread& t) {
        VaList a(t.x(2));
        emit(t, reinterpret_cast<char*>(t.x(0)), ~size_t(0), format_guest(reinterpret_cast<const char*>(t.x(1)), a));
    });
    guest::reg("vasprintf", [](Thread& t) {
        VaList a(t.x(2));
        std::string s = format_guest(reinterpret_cast<const char*>(t.x(1)), a);
        *reinterpret_cast<char**>(t.x(0)) = g_strdup(s.c_str());
        t.setx(0, s.size());
    });
    guest::reg("sscanf", [](Thread& t) {
        RegArgs a(t, 2);
        t.setx(0, static_cast<u64>(static_cast<i64>(scan_guest(reinterpret_cast<const char*>(t.x(0)), reinterpret_cast<const char*>(t.x(1)), a))));
    });
    guest::reg("vsscanf", [](Thread& t) {
        VaList a(t.x(2));
        t.setx(0, static_cast<u64>(static_cast<i64>(scan_guest(reinterpret_cast<const char*>(t.x(0)), reinterpret_cast<const char*>(t.x(1)), a))));
    });
    guest::reg("fscanf", [](Thread& t) {
        GFile* g = gf(reinterpret_cast<void*>(t.x(0)));
        std::string line;
        for (int c; (c = g_getc(t, g)) >= 0;) { line += static_cast<char>(c); if (c == '\n') break; }
        RegArgs a(t, 2);
        t.setx(0, line.empty() ? ~0ull : static_cast<u64>(static_cast<i64>(scan_guest(line.c_str(), reinterpret_cast<const char*>(t.x(1)), a))));
    });

    // descriptors
    IMPORT("open", [](const char* path, int flags) -> int {
        const char* mode = (flags & 3) == 0 ? "rb" : (flags & 0x200) ? "w+b" : (flags & 0x400) ? "a+b" : "r+b";
        GFile* g = open_guest(path, mode);
        if (!g && (flags & 0x40) && (flags & 3)) g = open_guest(path, "w+b");
        if (!g) { *guest_errno() = 2; log_once(std::string("open:") + (path ? path : ""), "[open] %s -> not found", path); return -1; }
        return new_fd(g);
    });
    IMPORT("close", [](Thread& t, int fd) -> int {
        GFile* g;
        { std::lock_guard lk(g_fd_mutex); auto it = g_fds.find(fd); if (it == g_fds.end()) return 0; g = it->second; g_fds.erase(it); }
        return g_close(t, g);
    });
    IMPORT("read", [](Thread& t, int fd, void* buf, size_t n) -> i64 {
        GFile* g = fd_file(fd);
        if (!g) { *guest_errno() = 11; return -1; }
        return static_cast<i64>(g_read(t, g, buf, n));
    });
    IMPORT("pread", [](Thread& t, int fd, void* buf, size_t n, i64 off) -> i64 {
        GFile* g = fd_file(fd);
        if (!g) return -1;
        i64 old = g_tell(t, g);
        g_seek(t, g, off, 0);
        size_t k = g_read(t, g, buf, n);
        g_seek(t, g, old, 0);
        return static_cast<i64>(k);
    });
    IMPORT("write", [](Thread& t, int fd, const void* buf, size_t n) -> i64 {
        if (fd == 1 || fd == 2) return static_cast<i64>(g_write(t, &g_std[fd], buf, n));
        GFile* g = fd_file(fd);
        return g ? static_cast<i64>(g_write(t, g, buf, n)) : static_cast<i64>(n);
    });
    IMPORT("lseek", [](Thread& t, int fd, i64 off, int whence) -> i64 {
        GFile* g = fd_file(fd);
        return g ? g_seek(t, g, off, whence) : -1;
    });
    IMPORT("fstat", [](Thread& t, int fd, BionicStat* st) -> int {
        GFile* g = fd_file(fd);
        if (!g) return -1;
        memset(st, 0, sizeof *st);
        st->mode = 0100644;
        i64 pos = g_tell(t, g);
        st->size = g_seek(t, g, 0, 2);
        g_seek(t, g, pos, 0);
        return 0;
    });

    // file system
    IMPORT("stat", [](const char* path, BionicStat* st) -> int {
        auto fake = fake_files().find(path ? path : "");
        if (fake != fake_files().end()) { memset(st, 0, sizeof *st); st->mode = 0100644; st->size = fake->second.size(); return 0; }
        return fill_stat(host_path(path), st);
    });
    IMPORT("access", [](const char* path, int) -> int {
        if (fake_files().count(path ? path : "")) return 0;
        std::string hp = host_path(path);
        if (!hp.empty() && fs::exists(hp)) return 0;
        *guest_errno() = 2;
        return -1;
    });
    IMPORT("mkdir", [](const char* path, int) -> int {
        std::string hp = host_path(path);
        std::error_code ec;
        if (hp.empty() || !fs::is_directory(fs::path(hp).parent_path(), ec)) { *guest_errno() = 2; return -1; }
        if (fs::is_directory(hp, ec)) { *guest_errno() = 17; return -1; }
        return fs::create_directory(hp, ec) ? 0 : -1;
    });
    auto rm = [](const char* path) -> int {
        std::string hp = host_path(path);
        std::error_code ec;
        return !hp.empty() && fs::remove(hp, ec) ? 0 : -1;
    };
    IMPORT("remove", rm); IMPORT("unlink", rm); IMPORT("rmdir", rm);
    IMPORT("rename", [](const char* a, const char* b) -> int {
        std::string ha = host_path(a), hb = host_path(b);
        std::error_code ec;
        if (ha.empty() || hb.empty()) return -1;
        fs::rename(ha, hb, ec);
        return ec ? -1 : 0;
    });
    IMPORT("chmod", [](const char*, int) -> int { return 0; });
    IMPORT("getcwd", [](char* buf, size_t n) -> char* { strncpy(buf, guest_cwd().c_str(), n); return buf; });
    IMPORT("chdir", [](const char* path) -> int { return set_guest_cwd(path) ? 0 : -1; });
    IMPORT("realpath", [](const char* path, char* out) -> char* {
        if (!out) out = static_cast<char*>(g_malloc(4096));
        strcpy(out, path);
        return out;
    });
    IMPORT("statfs", [](const char*, i64* buf) -> int {
        const i64 v[15] = {0xEF53, 4096, 10000000, 5000000, 5000000, 1000000, 500000, 0, 255, 4096};
        memcpy(buf, v, sizeof v);
        return 0;
    });
    IMPORT("opendir", [](const char* path) -> void* {
        std::string hp = host_path(path);
        std::error_code ec;
        if (hp.empty() || !fs::is_directory(hp, ec)) { *guest_errno() = 2; return nullptr; }
        auto* d = new GDir;
        d->it = fs::directory_iterator(hp, ec);
        return d;
    });
    IMPORT("readdir", [](GDir* d) -> BionicDirent* {
        if (!d || d->it == d->end) return nullptr;
        std::string name = d->it->path().filename().string();
        std::error_code ec;
        d->ent = {};
        d->ent.ino = 1;
        d->ent.reclen = sizeof(BionicDirent);
        d->ent.type = d->it->is_directory(ec) ? 4 : 8;
        strncpy(d->ent.name, name.c_str(), 255);
        d->it.increment(ec);
        return &d->ent;
    });
    IMPORT("closedir", [](GDir* d) -> int { delete d; return 0; });
    IMPORT("mkstemp", [](char*) -> int { return -1; });
    IMPORT("tmpnam", [](char*) -> char* { return nullptr; });
}

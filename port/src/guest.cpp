#include "guest.h"
#include "interp.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

using Dynarmic::A64::Vector;

// ---- logging -----------------------------------------------------------------
static std::mutex g_log_mutex;
static FILE* g_log_file = nullptr;

static void vlog(const char* fmt, va_list ap) {
    std::lock_guard lk(g_log_mutex);
    if (!g_log_file) g_log_file = fopen((project_root() + "/work/port.log").c_str(), "w");
    va_list ap2;
    va_copy(ap2, ap);
    static const auto start = std::chrono::steady_clock::now();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    fprintf(stdout, "%7.2f ", now);
    if (g_log_file) fprintf(g_log_file, "%7.2f ", now);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    if (g_log_file) {
        vfprintf(g_log_file, fmt, ap2);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
    va_end(ap2);
}

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void log_once(const std::string& key, const char* fmt, ...) {
    static std::set<std::string> seen;
    {
        std::lock_guard lk(g_log_mutex);
        if (!seen.insert(key).second) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap);
    va_end(ap);
    if (Thread* t = Thread::current_or_null()) guest::backtrace(*t);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

std::string project_root() {
    static std::string root = [] {
        char buf[MAX_PATH];
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        std::string p = buf;
        for (auto& c : p) if (c == '\\') c = '/';
        // The executable lives in <root>/port/bin.
        for (int i = 0; i < 3; i++) p = p.substr(0, p.find_last_of('/'));
        return p;
    }();
    return root;
}

// ---- stubs and imports -----------------------------------------------------------
namespace {
constexpr size_t STUB_AREA = 1 << 20;

u8* g_stub_area = nullptr;
std::vector<Handler> g_handlers;          // indexed by SVC number
std::vector<u8> g_inline;                 // 1 = handler may run inside the JIT's SVC callback (no JIT exit)

// Handlers that never call back into guest code can run straight from the JIT's
// service-call callback, which avoids leaving and re-entering translated code.
// Anything that might re-enter (qsort comparators, stream callbacks, pthread_once,
// JNI) or that ends the thread must take the slow path.
bool inline_capable(const std::string& name) {
    static const char* const names[] = {
        "memset", "memcpy", "memmove", "memcmp", "memchr", "malloc", "calloc", "realloc", "free", "memalign",
        "strlen", "strnlen", "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strcpy", "strncpy", "strcat", "strncat",
        "strchr", "strrchr", "strstr", "strspn", "strcspn", "strpbrk", "strdup", "strtol", "strtoul", "strtoll",
        "strtoull", "strtod", "strtof", "atoi", "atol", "atof", "isalnum", "isalpha", "isspace", "isupper", "islower",
        "isxdigit", "iscntrl", "ispunct", "tolower", "toupper", "pthread_mutex_lock", "pthread_mutex_unlock",
        "pthread_mutex_trylock", "pthread_getspecific", "pthread_setspecific", "pthread_self", "pthread_equal",
        "gettid", "clock_gettime", "gettimeofday", "time", "__errno", "rand", "snprintf", "sprintf", "vsnprintf",
        "vsprintf", "sscanf", "sin", "sinf", "cos", "cosf", "tan", "tanf", "asin", "asinf", "acos", "acosf", "atan",
        "atanf", "atan2", "atan2f", "exp", "expf", "exp2", "exp2f", "log", "logf", "log10", "log10f", "pow", "powf",
        "sqrt", "sqrtf", "fmod", "fmodf", "sinh", "sinhf", "cosh", "tanh", "ldexp", "ldexpf", "frexp", "modf", "modff"};
    if (name.rfind("gl", 0) == 0 && name.size() > 2 && name[2] >= 'A' && name[2] <= 'Z') return true;   // OpenGL ES
    for (const char* n : names) if (name == n) return true;
    return false;
}
std::vector<std::string> g_stub_names;
std::mutex g_stub_mutex;
u64 g_return_stub = 0;

std::unordered_map<std::string, Handler> g_imports;
struct DataImport { size_t size; std::function<void(u8*)> init; u64 addr = 0; };
std::unordered_map<std::string, DataImport> g_data_imports;

u64 g_base = 0, g_size = 0;
std::vector<u8> g_file;
std::unordered_map<std::string, u64> g_exports;
std::vector<std::pair<u64, std::string>> g_funcs;      // sorted, for symbolize
std::vector<u64> g_init_array;

Dynarmic::ExclusiveMonitor g_monitor(256);
std::atomic<int> g_next_cpu{0};
std::atomic<int> g_next_tid{1};
thread_local Thread* tl_current = nullptr;
u64 g_svc_counts[0x10000];          // calls per import (unsynchronised: statistics only)
std::mutex g_all_mutex;
std::vector<Thread*> g_all_threads;
}  // namespace

bool guest::trace = false;

u64 guest::make_stub(const std::string& name, Handler h) {
    std::lock_guard lk(g_stub_mutex);
    if (!g_stub_area) {
        g_stub_area = static_cast<u8*>(VirtualAlloc(nullptr, STUB_AREA, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        g_handlers.reserve(STUB_AREA / 8);      // never reallocates: other threads index it lock-free
        g_stub_names.reserve(STUB_AREA / 8);
        g_inline.reserve(STUB_AREA / 8);
    }
    u32 n = static_cast<u32>(g_handlers.size());
    if (n >= 0xFFFF) fatal("out of stubs");
    g_handlers.push_back(std::move(h));
    g_stub_names.push_back(name);
    g_inline.push_back(inline_capable(name) && !getenv("NOVA_NO_INLINE"));
    u32* code = reinterpret_cast<u32*>(g_stub_area + 8 * n);
    code[0] = 0xD4000001 | (n << 5);   // svc #n
    code[1] = 0xD65F03C0;              // ret
    return reinterpret_cast<u64>(code);
}

void guest::reg(const char* name, Handler h) { g_imports[name] = std::move(h); }
Handler guest::find_import(const std::string& name) {
    auto it = g_imports.find(name);
    return it == g_imports.end() ? Handler() : it->second;
}

void guest::reg_data(const char* name, size_t size, std::function<void(u8*)> init) {
    g_data_imports[name] = DataImport{size, std::move(init)};
}

u64 guest::data_addr(const char* name) {
    auto it = g_data_imports.find(name);
    return it == g_data_imports.end() ? 0 : it->second.addr;
}

u64 guest::sym(const std::string& name) {
    auto it = g_exports.find(name);
    return it == g_exports.end() ? 0 : it->second;
}

std::vector<std::pair<std::string, u64>> guest::exports_containing(const std::string& part) {
    std::vector<std::pair<std::string, u64>> out;
    for (auto& [name, addr] : g_exports)
        if (name.find(part) != std::string::npos) out.emplace_back(name, addr);
    return out;
}

const std::vector<u64>& guest::init_array() { return g_init_array; }
u64 guest::image_base() { return g_base; }
u64 guest::image_size() { return g_size; }
const std::vector<u8>& guest::image_file() { return g_file; }

std::string guest::symbolize(u64 addr) {
    char buf[64];
    if (g_stub_area && addr >= reinterpret_cast<u64>(g_stub_area) && addr < reinterpret_cast<u64>(g_stub_area) + STUB_AREA) {
        size_t n = (addr - reinterpret_cast<u64>(g_stub_area)) / 8;
        return n < g_stub_names.size() ? "stub:" + g_stub_names[n] : "stub:?";
    }
    if (addr >= g_base && addr < g_base + g_size) {
        auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), std::make_pair(addr, std::string("\x7f")));
        if (it != g_funcs.begin()) {
            --it;
            snprintf(buf, sizeof buf, "+0x%llx", static_cast<unsigned long long>(addr - it->first));
            return it->second + buf;
        }
        snprintf(buf, sizeof buf, "libNOVA+0x%llx", static_cast<unsigned long long>(addr - g_base));
        return buf;
    }
    snprintf(buf, sizeof buf, "0x%llx", static_cast<unsigned long long>(addr));
    return buf;
}

// ---- ELF loading -------------------------------------------------------------------
namespace {
struct Phdr { u32 type, flags; u64 off, va, pa, filesz, memsz, align; };
struct Dyn { i64 tag; u64 val; };
struct Sym { u32 name; u8 info, other; u16 shndx; u64 value, size; };
struct Rela { u64 off, info; i64 addend; };
}  // namespace

u64 guest::load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) fatal("cannot open %s", path.c_str());
    fseek(f, 0, SEEK_END);
    g_file.resize(ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(g_file.data(), 1, g_file.size(), f);
    fclose(f);
    const u8* d = g_file.data();

    u64 phoff = *reinterpret_cast<const u64*>(d + 0x20);
    u16 phnum = *reinterpret_cast<const u16*>(d + 0x38);
    const Phdr* ph = reinterpret_cast<const Phdr*>(d + phoff);
    u64 top = 0;
    const Phdr* dynph = nullptr;
    for (int i = 0; i < phnum; i++) {
        if (ph[i].type == 1) top = std::max(top, ph[i].va + ph[i].memsz);
        if (ph[i].type == 2) dynph = &ph[i];
    }
    g_size = (top + 0xFFFF) & ~0xFFFFull;
    g_base = reinterpret_cast<u64>(VirtualAlloc(nullptr, g_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_base) fatal("cannot allocate image");
    for (int i = 0; i < phnum; i++)
        if (ph[i].type == 1) memcpy(reinterpret_cast<void*>(g_base + ph[i].va), d + ph[i].off, ph[i].filesz);

    u64 symtab = 0, strtab = 0, rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, initarr = 0, initarrsz = 0, hash = 0;
    const Dyn* dyn = reinterpret_cast<const Dyn*>(g_base + dynph->va);
    for (; dyn->tag; dyn++) {
        switch (dyn->tag) {
        case 6: symtab = dyn->val; break;
        case 5: strtab = dyn->val; break;
        case 7: rela = dyn->val; break;
        case 8: relasz = dyn->val; break;
        case 23: jmprel = dyn->val; break;
        case 2: pltrelsz = dyn->val; break;
        case 25: initarr = dyn->val; break;
        case 27: initarrsz = dyn->val; break;
        case 4: hash = dyn->val; break;
        }
    }
    const Sym* syms = reinterpret_cast<const Sym*>(g_base + symtab);
    const char* strs = reinterpret_cast<const char*>(g_base + strtab);
    u32 nsyms = reinterpret_cast<const u32*>(g_base + hash)[1];   // nchain

    for (u32 i = 0; i < nsyms; i++) {
        if (syms[i].shndx && syms[i].name) {
            std::string name = strs + syms[i].name;
            g_exports[name] = g_base + syms[i].value;
            if ((syms[i].info & 15) == 2) g_funcs.emplace_back(g_base + syms[i].value, name);
        }
    }
    std::sort(g_funcs.begin(), g_funcs.end());

    std::unordered_map<u32, u64> resolved;
    auto resolve = [&](u32 idx) -> u64 {
        const Sym& s = syms[idx];
        if (s.shndx) return g_base + s.value;
        auto cached = resolved.find(idx);
        if (cached != resolved.end()) return cached->second;
        std::string name = strs + s.name;
        u64 addr;
        auto di = g_data_imports.find(name);
        if (di != g_data_imports.end() || (s.info & 15) == 1) {
            size_t size = di != g_data_imports.end() ? di->second.size : 0x400;
            u8* p = static_cast<u8*>(calloc(1, size));
            if (di != g_data_imports.end()) {
                if (di->second.init) di->second.init(p);
                di->second.addr = reinterpret_cast<u64>(p);
            } else {
                logf("!! imported data symbol %s left zeroed", name.c_str());
            }
            addr = reinterpret_cast<u64>(p);
        } else {
            auto it = g_imports.find(name);
            if (it != g_imports.end()) {
                addr = make_stub(name, it->second);
            } else {
                addr = make_stub(name, [name](Thread& t) {
                    log_once("unimpl:" + name, "!! unimplemented import %s (from %s)", name.c_str(),
                             symbolize(t.lr()).c_str());
                    t.setx(0, 0);
                });
            }
        }
        resolved[idx] = addr;
        return addr;
    };

    auto apply = [&](u64 tab, u64 size) {
        const Rela* r = reinterpret_cast<const Rela*>(g_base + tab);
        for (u64 i = 0; i < size / sizeof(Rela); i++) {
            u32 type = static_cast<u32>(r[i].info), symidx = static_cast<u32>(r[i].info >> 32);
            u64* where = reinterpret_cast<u64*>(g_base + r[i].off);
            switch (type) {
            case 1027: *where = g_base + r[i].addend; break;
            case 257: case 1025: case 1026: *where = resolve(symidx) + r[i].addend; break;
            default: fatal("unsupported relocation %u", type);
            }
        }
    };
    if (rela) apply(rela, relasz);
    if (jmprel) apply(jmprel, pltrelsz);

    for (u64 i = 0; i < initarrsz / 8; i++) g_init_array.push_back(reinterpret_cast<u64*>(g_base + initarr)[i]);
    return g_base;
}

// ---- CPU ----------------------------------------------------------------------------
struct Thread::Impl final : Dynarmic::A64::UserCallbacks {
    Thread* owner = nullptr;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
    Cpu cpu;                       // register state while the interpreter is running this thread
    bool in_interp = false;        // which of cpu / jit currently holds the registers

    void to_jit() {
        std::array<u64, 31> regs;
        memcpy(regs.data(), cpu.x, sizeof cpu.x);
        jit->SetRegisters(regs);
        jit->SetSP(cpu.sp);
        jit->SetPC(cpu.pc);
        jit->SetPstate(cpu.nzcv << 28);
        jit->SetFpcr(cpu.fpcr);
        jit->SetFpsr(cpu.fpsr);
        jit->SetVectors(cpu.v);
        in_interp = false;
    }
    void to_interp() {
        auto regs = jit->GetRegisters();
        memcpy(cpu.x, regs.data(), sizeof cpu.x);
        cpu.sp = jit->GetSP();
        cpu.pc = jit->GetPC();
        cpu.nzcv = jit->GetPstate() >> 28;
        cpu.fpcr = jit->GetFpcr();
        cpu.fpsr = jit->GetFpsr();
        cpu.v = jit->GetVectors();
        in_interp = true;
    }
    bool ShouldDeferTranslation(u64 pc) override { return interp::enabled && !owner->jit_only && !interp::is_hot(pc); }
    u8 tls_block[0x200] = {};

    template <class T> static T rd(u64 a) { T v; memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
    template <class T> static void wr(u64 a, T v) { memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

    std::unordered_set<u64> unique_pcs;       // only filled when NOVA_JIT_STATS is set
    std::optional<u32> MemoryReadCode(u64 a) override {
        owner->translated++;
        static const bool stats = getenv("NOVA_JIT_STATS") != nullptr;
        if (stats) unique_pcs.insert(a);
        return rd<u32>(a);
    }
    u8 MemoryRead8(u64 a) override { return rd<u8>(a); }
    u16 MemoryRead16(u64 a) override { return rd<u16>(a); }
    u32 MemoryRead32(u64 a) override { return rd<u32>(a); }
    u64 MemoryRead64(u64 a) override { return rd<u64>(a); }
    Vector MemoryRead128(u64 a) override { return rd<Vector>(a); }
    void MemoryWrite8(u64 a, u8 v) override { wr(a, v); }
    void MemoryWrite16(u64 a, u16 v) override { wr(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { wr(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { wr(a, v); }
    void MemoryWrite128(u64 a, Vector v) override { wr(a, v); }

    template <class T> static bool cas(u64 a, T value, T expected) {
        return reinterpret_cast<std::atomic<T>*>(a)->compare_exchange_strong(expected, value);
    }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return cas(a, v, e); }
    bool MemoryWriteExclusive128(u64 a, Vector v, Vector e) override {
        // No lock-free 128-bit CAS needed in practice; do it under a global lock.
        static std::mutex m;
        std::lock_guard lk(m);
        if (memcmp(reinterpret_cast<void*>(a), &e, 16) != 0) return false;
        memcpy(reinterpret_cast<void*>(a), &v, 16);
        return true;
    }

    void InterpreterFallback(u64 pc, size_t) override {
        fatal("** unsupported instruction %08x at %s", rd<u32>(pc), guest::symbolize(pc).c_str());
    }
    void CallSVC(u32 swi) override;
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception ex) override {
        fatal("** CPU exception %d at %s (instruction %08x)", static_cast<int>(ex), guest::symbolize(pc).c_str(),
              rd<u32>(pc));
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 1ull << 40; }
    u64 GetCNTPCT() override {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<u64>(c.QuadPart);
    }
};

void Thread::Impl::CallSVC(u32 swi) {
    if (swi < g_inline.size() && g_inline[swi] && !guest::trace) {
        // Registers are already in the JIT state at a service call (it ends the block).
        g_svc_counts[swi]++;
        owner->last_svc = swi;
        owner->svc_count++;
        g_handlers[swi](*owner);
        return;
    }
    owner->pending_svc = swi;
    jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
}

Thread::Thread(const std::string& name_, size_t stack_size_, size_t code_cache_mb) : name(name_), impl(new Impl) {
    tid = g_next_tid++;
    stack_size = std::max<size_t>(stack_size_, 1 << 20);
    stack = VirtualAlloc(nullptr, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    impl->owner = this;
    tpidr = reinterpret_cast<u64>(impl->tls_block) + 0x100;
    u64 guard = 0x5AFE5AFE5AFE5AFEull;
    memcpy(reinterpret_cast<void*>(tpidr + 0x28), &guard, 8);     // bionic's stack-protector slot

    Dynarmic::A64::UserConfig cfg;
    cfg.callbacks = impl.get();
    cfg.processor_id = static_cast<size_t>(g_next_cpu++ % 256);
    cfg.global_monitor = &g_monitor;
    cfg.tpidr_el0 = &tpidr;
    cfg.fastmem_pointer = 0;                 // identity mapping: guest address == host address
    cfg.fastmem_address_space_bits = 64;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = true;
    cfg.recompile_on_fastmem_failure = false;
    cfg.recompile_on_exclusive_fastmem_failure = false;
    cfg.enable_cycle_counting = false;
    cfg.wall_clock_cntpct = true;
    cfg.dczid_el0 = 0x10;                    // DC ZVA not available
    cfg.define_unpredictable_behaviour = true;
    cfg.code_cache_size = code_cache_mb << 20;
    // Faster floating point: skip exact-NaN bookkeeping and FMA fusing, as console
    // emulators do by default. Set NOVA_SAFE_FP=1 to turn this off.
    if (!getenv("NOVA_SAFE_FP")) {
        cfg.unsafe_optimizations = true;
        cfg.optimizations |= Dynarmic::OptimizationFlag::Unsafe_UnfuseFMA | Dynarmic::OptimizationFlag::Unsafe_ReducedErrorFP |
                             Dynarmic::OptimizationFlag::Unsafe_InaccurateNaN;
    }
    // The fast-dispatch table costs 16 MB per JIT. Background threads (there are ~40)
    // run little code, so only the big threads get one.
    if (code_cache_mb < 96) cfg.optimizations &= ~Dynarmic::OptimizationFlag::FastDispatch;
    if (const char* opt = getenv("NOVA_JIT_OPT")) {          // experiment: e.g. 0 = no IR optimisation passes
        cfg.optimizations = static_cast<Dynarmic::OptimizationFlag>(strtoul(opt, nullptr, 0));
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    cfg.cntfrq_el0 = static_cast<u32>(freq.QuadPart);
    impl->jit = std::make_unique<Dynarmic::A64::Jit>(cfg);
    impl->jit->SetSP(reinterpret_cast<u64>(stack) + stack_size - 0x100);
    impl->cpu.sp = reinterpret_cast<u64>(stack) + stack_size - 0x100;
    impl->in_interp = interp::enabled;
    std::lock_guard lk(g_all_mutex);
    g_all_threads.push_back(this);
}

Thread::~Thread() {
    if (stack) VirtualFree(stack, 0, MEM_RELEASE);
}

u64 Thread::unique_translated() const { return impl->unique_pcs.size(); }
u64 Thread::x(int n) const { return impl->in_interp ? impl->cpu.x[n] : impl->jit->GetRegister(n); }
void Thread::setx(int n, u64 v) { if (impl->in_interp) impl->cpu.x[n] = v; else impl->jit->SetRegister(n, v); }
u64 Thread::sp() const { return impl->in_interp ? impl->cpu.sp : impl->jit->GetSP(); }
void Thread::set_sp(u64 v) { if (impl->in_interp) impl->cpu.sp = v; else impl->jit->SetSP(v); }
u64 Thread::pc() const { return impl->in_interp ? impl->cpu.pc : impl->jit->GetPC(); }
void Thread::set_pc(u64 v) { if (impl->in_interp) impl->cpu.pc = v; else impl->jit->SetPC(v); }
static Vector get_v(const Thread::Impl& i, int n) { return i.in_interp ? i.cpu.v[n] : i.jit->GetVector(n); }
static void set_v(Thread::Impl& i, int n, Vector v) { if (i.in_interp) i.cpu.v[n] = v; else i.jit->SetVector(n, v); }
double Thread::d(int n) const { double v; u64 lo = get_v(*impl, n)[0]; memcpy(&v, &lo, 8); return v; }
float Thread::s(int n) const { float v; u64 lo = get_v(*impl, n)[0]; memcpy(&v, &lo, 4); return v; }
void Thread::setd(int n, double v) { u64 lo; memcpy(&lo, &v, 8); set_v(*impl, n, Vector{lo, 0}); }
void Thread::sets(int n, float v) { u64 lo = 0; memcpy(&lo, &v, 4); set_v(*impl, n, Vector{lo, 0}); }

Thread& Thread::current() {
    if (!tl_current) fatal("no guest thread bound to this host thread");
    return *tl_current;
}
Thread* Thread::current_or_null() { return tl_current; }
void Thread::make_current() { tl_current = this; }

u64 Thread::call(u64 fn, std::initializer_list<u64> args) {
    if (!g_return_stub) g_return_stub = guest::make_stub("<return>", [](Thread&) {});
    Thread* prev = tl_current;
    tl_current = this;
    u64 saved_lr = x(30), saved_pc = pc(), saved_sp = sp();
    int i = 0;
    for (u64 a : args) setx(i++, a);
    set_sp((saved_sp - 0x100) & ~0xFull);     // stay clear of the interrupted frame
    setx(30, g_return_stub);
    set_pc(fn);
    for (;;) {
        pending_svc = ~0u;
        if (impl->in_interp) {
            u32 svc = 0;
            if (interp::run(impl->cpu, tpidr, &svc, &interpreted) == interp::Stop::WantJit) {
                impl->to_jit();
                continue;
            }
            pending_svc = svc;
        } else {
            Dynarmic::HaltReason hr = impl->jit->Run();
            if (Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined3)) {       // --guest-profile sample
                guest_samples[pc()]++;
                if (pending_svc == ~0u && !Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) continue;
            }
            if (pending_svc == ~0u && Dynarmic::Has(hr, Dynarmic::HaltReason::UserDefined2)) {
                impl->to_interp();          // reached code that is still cold
                continue;
            }
            if (pending_svc == ~0u)
                fatal("** JIT stopped without a service call at %s (halt reason %08x)", guest::symbolize(pc()).c_str(),
                      static_cast<u32>(hr));
        }
        u32 n = pending_svc;
        if (n == ~0u) fatal("** JIT stopped without a service call at %s", guest::symbolize(pc()).c_str());
        if (pc() == g_return_stub + 4) break;
        if (n >= g_handlers.size()) fatal("** bad service call %u", n);
        if (guest::trace)
            logf("[%d] %s(%llx, %llx, %llx) from %s", tid, g_stub_names[n].c_str(), x(0), x(1), x(2),
                 guest::symbolize(lr()).c_str());
        g_svc_counts[n]++;
        last_svc = n;
        last_lr = lr();
        svc_count++;
        g_handlers[n](*this);
        if (exited) break;
    }
    u64 result = x(0);
    setx(30, saved_lr);
    set_pc(saved_pc);
    set_sp(saved_sp);
    tl_current = prev ? prev : this;
    return result;
}

void guest::backtrace(Thread& t) {
    logf("   thread %d (%s) pc=%s", t.tid, t.name.c_str(), symbolize(t.pc()).c_str());
    logf("   lr=%s", symbolize(t.lr()).c_str());
    u64 fp = t.x(29);
    u64 lo = reinterpret_cast<u64>(t.stack), hi = lo + t.stack_size;
    for (int i = 0; i < 16 && fp >= lo && fp + 16 <= hi; i++) {
        u64 next, ret;
        memcpy(&next, reinterpret_cast<void*>(fp), 8);
        memcpy(&ret, reinterpret_cast<void*>(fp + 8), 8);
        logf("   <- %s", symbolize(ret).c_str());
        if (next <= fp) break;
        fp = next;
    }
}

void guest::dump_threads() {
    std::lock_guard lk(g_all_mutex);
    {
        std::vector<std::pair<u64, u32>> top;
        u64 total = 0;
        for (u32 i = 0; i < g_stub_names.size(); i++) if (g_svc_counts[i]) { top.emplace_back(g_svc_counts[i], i); total += g_svc_counts[i]; }
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 24; i++)
            line += g_stub_names[top[i].second] + " " + std::to_string(top[i].first * 100 / std::max<u64>(total, 1)) + "%  ";
        logf("--- %llu import calls; most frequent: %s", static_cast<unsigned long long>(total), line.c_str());
    }
    logf("--- guest threads ---");
    for (Thread* t : g_all_threads) {
        std::string last = t->last_svc < g_stub_names.size() ? g_stub_names[t->last_svc] : "-";
        logf("  [%d] %-32.32s %s imports=%llu interpreted=%llu translated=%llu last=%s from %.60s", t->tid, t->name.c_str(),
             t->exited ? "exited " : "running", static_cast<unsigned long long>(t->svc_count),
             static_cast<unsigned long long>(t->interpreted), static_cast<unsigned long long>(t->translated), last.c_str(),
             symbolize(t->last_lr).c_str());
    }
}

// ---- interpreter self-check: replay each interpreted instruction on the JIT and compare ----
static void verify_step(const Cpu& before, const Cpu& after, u32 insn) {
    static thread_local Thread* vt = nullptr;
    static std::atomic<int> reported{0};
    static std::atomic<u64> checked{0};
    if (!vt) {
        vt = new Thread("interp-verifier", 1 << 20, 32);
        vt->jit_only = true;
        vt->impl->in_interp = false;
    }
    Thread::Impl& v = *vt->impl;
    vt->tpidr = Thread::current().tpidr;
    v.cpu = before;
    v.to_jit();
    v.jit->Step();
    Cpu got;
    {
        auto regs = v.jit->GetRegisters();
        memcpy(got.x, regs.data(), sizeof got.x);
        got.sp = v.jit->GetSP();
        got.pc = v.jit->GetPC();
        got.nzcv = v.jit->GetPstate() >> 28;
        got.v = v.jit->GetVectors();
    }
    u64 n = ++checked;
    if ((n & (n - 1)) == 0 && n >= (1 << 20)) logf("[verify] %llu instructions checked", static_cast<unsigned long long>(n));
    std::string diff;
    char buf[160];
    for (int i = 0; i < 31; i++)
        if (got.x[i] != after.x[i]) { snprintf(buf, sizeof buf, " x%d: interp %llx jit %llx;", i, after.x[i], got.x[i]); diff += buf; }
    if (got.sp != after.sp) { snprintf(buf, sizeof buf, " sp: interp %llx jit %llx;", after.sp, got.sp); diff += buf; }
    if (got.pc != after.pc) { snprintf(buf, sizeof buf, " pc: interp %llx jit %llx;", after.pc, got.pc); diff += buf; }
    if (got.nzcv != after.nzcv) { snprintf(buf, sizeof buf, " nzcv: interp %x jit %x;", after.nzcv, got.nzcv); diff += buf; }
    for (int i = 0; i < 32; i++)
        if (got.v[i] != after.v[i]) { snprintf(buf, sizeof buf, " v%d: interp %016llx%016llx jit %016llx%016llx;", i, after.v[i][1], after.v[i][0], got.v[i][1], got.v[i][0]); diff += buf; }
    if (!diff.empty() && reported++ < 60)
        logf("[verify] MISMATCH insn %08x at %s:%s", insn, guest::symbolize(before.pc).c_str(), diff.c_str());
}

void guest::enable_interp_verify() { interp::verify = verify_step; }

// ---- guest profiler: periodically interrupt a thread's JIT and note where it was ----
void guest::start_guest_profile(int tid) {
    std::thread([tid] {
        for (;;) {
            Sleep(1);
            std::lock_guard lk(g_all_mutex);
            for (Thread* t : g_all_threads)
                if (t->tid == tid && !t->exited) t->impl->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
        }
    }).detach();
}

void guest::report_guest_profile(int tid) {
    std::lock_guard lk(g_all_mutex);
    for (Thread* t : g_all_threads) {
        if (t->tid != tid) continue;
        std::map<std::string, u64> by_fn;
        u64 total = 0;
        for (auto& [pc, n] : t->guest_samples) {
            std::string name = symbolize(pc);
            by_fn[name.substr(0, name.rfind('+'))] += n;
            total += n;
        }
        std::vector<std::pair<u64, std::string>> v;
        for (auto& [n, c] : by_fn) v.emplace_back(c, n);
        std::sort(v.rbegin(), v.rend());
        std::map<u64, u64> by_region;                 // 256-byte buckets, as offsets into the library
        for (auto& [pc, n] : t->guest_samples)
            if (pc >= g_base && pc < g_base + g_size) by_region[(pc - g_base) & ~0xFFull] += n;
        std::vector<std::pair<u64, u64>> r;
        for (auto& [off, n] : by_region) r.emplace_back(n, off);
        std::sort(r.rbegin(), r.rend());
        logf("--- hottest code regions (library offset) ---");
        for (size_t i = 0; i < r.size() && i < 24; i++)
            logf("  %5.1f%%  libNOVA+0x%llx", 100.0 * r[i].first / std::max<u64>(total, 1), static_cast<unsigned long long>(r[i].second));
        logf("--- guest profile of thread %d: %llu samples ---", tid, static_cast<unsigned long long>(total));
        for (size_t i = 0; i < v.size() && i < 40; i++)
            logf("  %5.1f%%  %.110s", 100.0 * v[i].first / std::max<u64>(total, 1), v[i].second.c_str());
    }
}

#include <psapi.h>
double guest::private_mb() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc);
    return pmc.PrivateUsage / 1048576.0;
}

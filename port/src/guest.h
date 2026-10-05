// Core of the loader: guest threads (one dynarmic JIT each), the ELF image,
// import stubs and argument marshalling between ARM64 guest and x64 host.
//
// Guest memory is identity-mapped: a guest pointer is a host pointer, so host
// functions can be handed guest pointers directly.
#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_map>
#include <type_traits>
#include <vector>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;
using i64 = int64_t;

namespace Dynarmic::A64 { class Jit; }

struct Thread;
using Handler = std::function<void(Thread&)>;

struct Thread {
    int tid = 0;
    std::string name;
    std::vector<u64> tls;            // pthread_getspecific slots
    u64 tpidr = 0;                   // TPIDR_EL0 value (points into tls_block)
    void* stack = nullptr;
    size_t stack_size = 0;
    u32 pending_svc = ~0u;
    u32 last_svc = ~0u;              // most recent import called (for thread dumps)
    u64 last_lr = 0;
    u64 svc_count = 0;
    std::unordered_map<u64, u64> guest_samples;   // --guest-profile: pc -> hits
    u64 interpreted = 0;             // guest instructions run by the interpreter tier
    bool jit_only = false;           // never defer to the interpreter (used by the verifier)
    u64 translated = 0;              // guest instructions translated by this thread's JIT
    double jit_seconds = 0;          // time spent inside translated code (incl. translating it)
    bool exited = false;
    u64 exit_value = 0;
    void* host_thread = nullptr;     // std::thread*, owned by pthread layer
    struct Impl;
    std::unique_ptr<Impl> impl;

    Thread(const std::string& name, size_t stack_size = 0, size_t code_cache_mb = 64);
    ~Thread();

    u64 unique_translated() const;
    u64 x(int n) const;
    void setx(int n, u64 v);
    u64 sp() const;
    void set_sp(u64 v);
    u64 pc() const;
    void set_pc(u64 v);
    u64 lr() const { return x(30); }
    double d(int n) const;
    float s(int n) const;
    void setd(int n, double v);
    void sets(int n, float v);

    // Call a guest function and run until it returns. Safe to use from inside
    // an import handler (nested).
    u64 call(u64 fn, std::initializer_list<u64> args = {});

    static Thread& current();
    static Thread* current_or_null();
    void make_current();
};

namespace guest {
u64 load(const std::string& so_path);              // maps and relocates; returns base
u64 sym(const std::string& name);
std::vector<std::pair<std::string, u64>> exports_containing(const std::string& part);                  // exported symbol, 0 if absent
std::string symbolize(u64 addr);
u64 make_stub(const std::string& name, Handler h); // address of a callable stub
void reg(const char* name, Handler h);
Handler find_import(const std::string& name);       // handler registered for an import, if any             // provide an imported function
void reg_data(const char* name, size_t size, std::function<void(u8*)> init = nullptr);
u64 data_addr(const char* name);                   // address given to an imported data symbol
const std::vector<u64>& init_array();
u64 image_base();
u64 image_size();
const std::vector<u8>& image_file();
void backtrace(Thread& t);
void enable_interp_verify();                         // slow self-check of the interpreter tier
void start_guest_profile(int tid);                  // sample where a guest thread spends its time
void report_guest_profile(int tid);
double private_mb();                               // committed private memory of the process
void dump_threads();                               // where every guest thread last called out
extern bool trace;
}

[[noreturn]] void fatal(const char* fmt, ...);
void logf(const char* fmt, ...);
void log_once(const std::string& key, const char* fmt, ...);

// ---- argument marshalling (AAPCS64 -> host) --------------------------------
struct ArgReader {
    Thread& t;
    int gp = 0, fp = 0;
    u64 next_gp() {
        int i = gp++;
        if (i < 8) return t.x(i);
        u64 v;
        std::memcpy(&v, reinterpret_cast<void*>(t.sp() + 8 * u64(i - 8)), 8);
        return v;
    }
    template <class T>
    T next() {
        if constexpr (std::is_same_v<T, float>) {
            return t.s(fp++);
        } else if constexpr (std::is_same_v<T, double>) {
            return t.d(fp++);
        } else if constexpr (std::is_pointer_v<T>) {
            return reinterpret_cast<T>(next_gp());
        } else if constexpr (std::is_enum_v<T>) {
            return static_cast<T>(next_gp());
        } else {
            return static_cast<T>(next_gp());
        }
    }
};

template <class R>
void set_result(Thread& t, R r) {
    if constexpr (std::is_same_v<R, float>) {
        t.sets(0, r);
    } else if constexpr (std::is_same_v<R, double>) {
        t.setd(0, r);
    } else if constexpr (std::is_pointer_v<R>) {
        t.setx(0, reinterpret_cast<u64>(r));
    } else if constexpr (std::is_signed_v<R>) {
        t.setx(0, static_cast<u64>(static_cast<i64>(r)));
    } else {
        t.setx(0, static_cast<u64>(r));
    }
}

// Wrap a host function so the guest can call it with its own calling convention.
template <class R, class... A>
Handler wrap(R (*f)(A...)) {
    return [f](Thread& t) {
        ArgReader r{t};
        std::tuple<std::decay_t<A>...> args{r.template next<std::decay_t<A>>()...};
        if constexpr (std::is_void_v<R>) {
            std::apply(f, args);
        } else {
            set_result<R>(t, std::apply(f, args));
        }
    };
}

// Same, for handlers that also want the calling thread as first parameter.
template <class R, class... A>
Handler wrap(R (*f)(Thread&, A...)) {
    return [f](Thread& t) {
        ArgReader r{t};
        std::tuple<std::decay_t<A>...> args{r.template next<std::decay_t<A>>()...};
        if constexpr (std::is_void_v<R>) {
            std::apply([&](auto&&... a) { f(t, a...); }, args);
        } else {
            set_result<R>(t, std::apply([&](auto&&... a) { return f(t, a...); }, args));
        }
    };
}

// Variadic so lambda bodies may contain top-level commas.
#define IMPORT(name, ...) guest::reg(name, wrap(+__VA_ARGS__))

// Subsystem registration (each file registers its imports).
void register_libc();
void register_stdio();
void register_pthread();
void register_android();
void register_gles();
void register_audio();
void register_net();
void install_hle();          // native replacements for a few hot guest functions

// Paths
std::string project_root();

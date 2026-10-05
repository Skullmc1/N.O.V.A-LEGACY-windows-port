// Shared helpers for the bionic libc replacement.
#pragma once
#include "guest.h"

// Variadic argument sources (AAPCS64).
struct VArgs {
    virtual u64 gp() = 0;
    virtual double fp() = 0;
    virtual ~VArgs() = default;
};

// Arguments still in registers / on the stack at an import call.
struct RegArgs final : VArgs {
    Thread& t;
    int g, f = 0;
    u64 stack;
    RegArgs(Thread& t_, int first_gp) : t(t_), g(first_gp), stack(t_.sp()) {}
    u64 pop() { u64 v; memcpy(&v, reinterpret_cast<void*>(stack), 8); stack += 8; return v; }
    u64 gp() override { return g < 8 ? t.x(g++) : pop(); }
    double fp() override {
        if (f < 8) return t.d(f++);
        u64 v = pop(); double d; memcpy(&d, &v, 8); return d;
    }
};

// A guest va_list: { void* stack; void* gr_top; void* vr_top; int gr_offs; int vr_offs; }
struct VaList final : VArgs {
    u64 stack, gr_top, vr_top;
    i32 gr_offs, vr_offs;
    explicit VaList(u64 p) {
        memcpy(&stack, reinterpret_cast<void*>(p), 8);
        memcpy(&gr_top, reinterpret_cast<void*>(p + 8), 8);
        memcpy(&vr_top, reinterpret_cast<void*>(p + 16), 8);
        memcpy(&gr_offs, reinterpret_cast<void*>(p + 24), 4);
        memcpy(&vr_offs, reinterpret_cast<void*>(p + 28), 4);
    }
    u64 pop() { u64 v; memcpy(&v, reinterpret_cast<void*>(stack), 8); stack += 8; return v; }
    u64 gp() override {
        if (gr_offs < 0) { u64 v; memcpy(&v, reinterpret_cast<void*>(gr_top + gr_offs), 8); gr_offs += 8; return v; }
        return pop();
    }
    double fp() override {
        u64 v;
        if (vr_offs < 0) { memcpy(&v, reinterpret_cast<void*>(vr_top + vr_offs), 8); vr_offs += 16; }
        else v = pop();
        double d; memcpy(&d, &v, 8); return d;
    }
};

std::string format_guest(const char* fmt, VArgs& args);
int scan_guest(const char* text, const char* fmt, VArgs& args);

// Guest heap (host heap with a small header so memalign'd blocks can be freed).
void* g_malloc(size_t n);
void* g_calloc(size_t n, size_t m);
void* g_realloc(void* p, size_t n);
void* g_memalign(size_t align, size_t n);
void g_free(void* p);
char* g_strdup(const char* s);

// Android paths -> files inside the sandbox (work/fs). Empty string if the
// path has no mapping.
extern const char* const PKG;
std::string host_path(const char* guest_path);
std::string guest_cwd();
bool set_guest_cwd(const char* path);
int* guest_errno();

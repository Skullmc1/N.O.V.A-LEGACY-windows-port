// Tier 0: a plain ARM64 interpreter for cold code.
//
// Translating a block with dynarmic costs microseconds per instruction, which
// is wasted on code that runs once (static initialisers, loading). Code starts
// here; a block that has been entered often enough, or that contains an
// instruction this interpreter does not implement, is handed to the JIT.
#pragma once
#include <array>

#include "guest.h"

struct Cpu {
    u64 x[31] = {};
    u64 sp = 0;
    u64 pc = 0;
    u32 nzcv = 0;                             // bit 3 = N, 2 = Z, 1 = C, 0 = V
    u32 fpcr = 0, fpsr = 0;
    std::array<std::array<u64, 2>, 32> v = {};
};

namespace interp {
enum class Stop { Svc, WantJit };

// Runs until a service call (svc number in *svc, pc already past it) or until
// the code at cpu.pc should run in the JIT.
Stop run(Cpu& cpu, u64 tpidr, u32* svc, u64* executed);

bool is_hot(u64 pc);          // true once a block start should be translated
void mark_hot(u64 pc);
extern bool enabled;
// Test hook (--verify-interp): called after every interpreted instruction with the state before and after.
extern void (*verify)(const Cpu& before, const Cpu& after, u32 insn);
extern u32 hot_threshold;
void report_unsupported();    // histogram of instructions that forced a JIT fallback (NOVA_JIT_STATS)
}

#include "interp.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

bool interp::enabled = true;
u32 interp::hot_threshold = 64;
void (*interp::verify)(const Cpu&, const Cpu&, u32) = nullptr;

// ---- hotness table: block start -> entry count (lock-free, shared by all threads) ----
namespace {
constexpr u32 HOT = 0x80000000u;
constexpr size_t SLOTS = size_t(1) << 21;
struct Slot { std::atomic<u64> pc; std::atomic<u32> count; };
Slot* g_slots = new Slot[SLOTS]();

Slot* find_slot(u64 pc) {
    size_t i = static_cast<size_t>((pc >> 2) * 0x9E3779B97F4A7C15ull >> 43) & (SLOTS - 1);
    for (size_t probes = 0; probes < 64; probes++, i = (i + 1) & (SLOTS - 1)) {
        u64 cur = g_slots[i].pc.load(std::memory_order_relaxed);
        if (cur == pc) return &g_slots[i];
        if (cur == 0) {
            u64 expected = 0;
            if (g_slots[i].pc.compare_exchange_strong(expected, pc) || expected == pc) return &g_slots[i];
        }
    }
    return nullptr;       // table crowded: treat as hot
}

// Counts an entry to the block at pc; true when it should go to the JIT.
inline bool enter_block(u64 pc) {
    Slot* s = find_slot(pc);
    if (!s) return true;
    u32 c = s->count.load(std::memory_order_relaxed);
    if (c & HOT) return true;
    if (c >= interp::hot_threshold) { s->count.store(c | HOT, std::memory_order_relaxed); return true; }
    s->count.store(c + 1, std::memory_order_relaxed);
    return false;
}

std::mutex g_unsupported_mutex;
std::map<u32, std::pair<u64, u32>> g_unsupported;      // class key -> (count, example)
bool g_stats = getenv("NOVA_JIT_STATS") != nullptr;
}  // namespace

bool interp::is_hot(u64 pc) {
    Slot* s = find_slot(pc);
    return !s || (s->count.load(std::memory_order_relaxed) & HOT);
}

void interp::mark_hot(u64 pc) {
    if (Slot* s = find_slot(pc)) s->count.fetch_or(HOT, std::memory_order_relaxed);
}

void interp::report_unsupported() {
    std::lock_guard lk(g_unsupported_mutex);
    std::vector<std::pair<u64, u32>> v;
    for (auto& [k, e] : g_unsupported) v.emplace_back(e.first, e.second);
    std::sort(v.rbegin(), v.rend());
    logf("--- instructions the interpreter handed to the JIT (count, example encoding) ---");
    for (size_t i = 0; i < v.size() && i < 40; i++) logf("  %8llu  %08x", static_cast<unsigned long long>(v[i].first), v[i].second);
}

// ---- helpers --------------------------------------------------------------------------
namespace {
template <class T> inline T rd(u64 a) { T v; memcpy(&v, reinterpret_cast<void*>(a), sizeof v); return v; }
template <class T> inline void wr(u64 a, T v) { memcpy(reinterpret_cast<void*>(a), &v, sizeof v); }

inline u32 bits(u32 insn, int hi, int lo) { return (insn >> lo) & ((1u << (hi - lo + 1)) - 1); }
inline i64 sext(u64 v, int width) { return static_cast<i64>(v << (64 - width)) >> (64 - width); }

inline u64 reg(const Cpu& c, u32 n) { return n == 31 ? 0 : c.x[n]; }
inline u64 regsp(const Cpu& c, u32 n) { return n == 31 ? c.sp : c.x[n]; }
inline void setreg(Cpu& c, u32 n, u64 v, bool sf) { if (n != 31) c.x[n] = sf ? v : static_cast<u32>(v); }
inline void setregsp(Cpu& c, u32 n, u64 v, bool sf) {
    if (!sf) v = static_cast<u32>(v);
    if (n == 31) c.sp = v; else c.x[n] = v;
}

inline u64 ror(u64 v, unsigned r, unsigned size) {
    r %= size;
    if (!r) return v;
    u64 mask = size == 64 ? ~0ull : (1ull << size) - 1;
    return ((v >> r) | (v << (size - r))) & mask;
}

inline u64 shift_op(u64 v, u32 type, u32 amount, bool sf) {
    if (!sf) v = static_cast<u32>(v);
    unsigned size = sf ? 64 : 32;
    if (amount == 0) return v;
    switch (type) {
    case 0: return sf ? v << amount : static_cast<u32>(v << amount);
    case 1: return v >> amount;
    case 2: return sf ? static_cast<u64>(static_cast<i64>(v) >> amount) : static_cast<u32>(static_cast<i32>(v) >> amount);
    default: return ror(v, amount, size);
    }
}

inline u64 extend_op(u64 v, u32 option, u32 shift) {
    switch (option) {
    case 0: v = static_cast<u8>(v); break;
    case 1: v = static_cast<u16>(v); break;
    case 2: v = static_cast<u32>(v); break;
    case 3: break;
    case 4: v = static_cast<u64>(static_cast<i64>(static_cast<int8_t>(v))); break;
    case 5: v = static_cast<u64>(static_cast<i64>(static_cast<int16_t>(v))); break;
    case 6: v = static_cast<u64>(static_cast<i64>(static_cast<i32>(v))); break;
    default: break;
    }
    return v << shift;
}

inline void set_nz(Cpu& c, u64 result, bool sf) {
    bool n = sf ? (result >> 63) : ((result >> 31) & 1);
    bool z = sf ? result == 0 : static_cast<u32>(result) == 0;
    c.nzcv = (c.nzcv & 3) | (n << 3) | (z << 2);
}

// x + y + carry with flags (used for ADD/SUB/CMP/ADC/SBC/CCMP).
inline u64 add_carry(Cpu* c, u64 x, u64 y, u32 carry, bool sf, bool setflags) {
    u64 result;
    bool cf, vf;
    if (sf) {
        u64 t = x + y;
        result = t + carry;
        cf = t < x || result < t;
        vf = (~(x ^ y) & (x ^ result)) >> 63;
    } else {
        u64 wide = static_cast<u64>(static_cast<u32>(x)) + static_cast<u32>(y) + carry;
        result = static_cast<u32>(wide);
        cf = wide >> 32;
        vf = ((~(static_cast<u32>(x) ^ static_cast<u32>(y)) & (static_cast<u32>(x) ^ static_cast<u32>(result))) >> 31) & 1;
    }
    if (setflags) {
        bool n = sf ? (result >> 63) : (result >> 31) & 1;
        c->nzcv = (n << 3) | ((result == 0) << 2) | (cf << 1) | static_cast<u32>(vf);
    }
    return result;
}

inline bool cond_holds(const Cpu& c, u32 cond) {
    bool n = c.nzcv & 8, z = c.nzcv & 4, cf = c.nzcv & 2, v = c.nzcv & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cf; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cf && !z; break;
    case 5: r = n == v; break;
    case 6: r = n == v && !z; break;
    default: return true;
    }
    return (cond & 1) ? !r : r;
}

// DecodeBitMasks from the ARM reference manual.
inline bool decode_bitmasks(u32 n, u32 imms, u32 immr, bool immediate, bool sf, u64* wmask, u64* tmask) {
    u32 combined = (n << 6) | (~imms & 0x3F);
    if (!combined) return false;
    unsigned long len;
    _BitScanReverse(&len, combined);
    if (len < 1) return false;
    u32 levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels) return false;
    u32 s = imms & levels, r = immr & levels;
    u32 d = (s - r) & levels;
    unsigned esize = 1u << len;
    if (!sf && esize == 64) return false;
    auto ones = [](unsigned k) { return k >= 64 ? ~0ull : (1ull << k) - 1; };
    u64 welem = ones(s + 1), telem = ones(d + 1);
    welem = ror(welem, r, esize);
    u64 w = 0, t = 0;
    for (unsigned i = 0; i < 64; i += esize) { w |= welem << i; t |= telem << i; }
    if (!sf) { w = static_cast<u32>(w); t = static_cast<u32>(t); }
    *wmask = w; *tmask = t;
    return true;
}

inline float vs(const Cpu& c, u32 n) { float f; memcpy(&f, &c.v[n][0], 4); return f; }
inline double vd(const Cpu& c, u32 n) { double d; memcpy(&d, &c.v[n][0], 8); return d; }
inline void set_vs(Cpu& c, u32 n, float f) { u64 lo = 0; memcpy(&lo, &f, 4); c.v[n] = {lo, 0}; }
inline void set_vd(Cpu& c, u32 n, double d) { u64 lo; memcpy(&lo, &d, 8); c.v[n] = {lo, 0}; }

enum { NEXT = 0, BRANCH = 1, SVC = 2, UNSUPPORTED = 3 };

// Executes one instruction. NEXT: fell through (pc advanced). BRANCH: control
// transfer or block end (pc set). SVC: service call (pc not advanced).
inline int exec(Cpu& c, u32 insn, u64 tpidr) {
    const u32 rd_ = insn & 31, rn = (insn >> 5) & 31;
    const bool sf = insn >> 31;
    switch ((insn >> 25) & 0xF) {
    // ---------------------------------------------------------------- data processing, immediate
    case 0x8: case 0x9: {
        switch ((insn >> 23) & 7) {
        case 0: case 1: {                                  // ADR / ADRP
            i64 imm = sext((static_cast<u64>(bits(insn, 23, 5)) << 2) | bits(insn, 30, 29), 21);
            if (sf) setreg(c, rd_, (c.pc & ~0xFFFull) + (static_cast<u64>(imm) << 12), true);
            else setreg(c, rd_, c.pc + imm, true);
            break;
        }
        case 2: {                                          // ADD/SUB immediate
            u64 imm = bits(insn, 21, 10);
            if (insn & (1u << 22)) imm <<= 12;
            bool sub = insn & (1u << 30), s = insn & (1u << 29);
            u64 a = regsp(c, rn);
            u64 r = sub ? add_carry(&c, a, ~imm, 1, sf, s) : add_carry(&c, a, imm, 0, sf, s);
            if (s) setreg(c, rd_, r, sf); else setregsp(c, rd_, r, sf);
            break;
        }
        case 4: {                                          // logical immediate
            u64 wmask, tmask;
            if (!sf && (insn & (1u << 22))) return UNSUPPORTED;
            if (!decode_bitmasks(bits(insn, 22, 22), bits(insn, 15, 10), bits(insn, 21, 16), true, sf, &wmask, &tmask)) return UNSUPPORTED;
            u64 a = reg(c, rn), r;
            u32 opc = bits(insn, 30, 29);
            r = opc == 1 ? a | wmask : opc == 2 ? a ^ wmask : a & wmask;
            if (!sf) r = static_cast<u32>(r);
            if (opc == 3) { set_nz(c, r, sf); c.nzcv &= ~3u; setreg(c, rd_, r, sf); }
            else setregsp(c, rd_, r, sf);
            break;
        }
        case 5: {                                          // MOVN / MOVZ / MOVK
            u32 opc = bits(insn, 30, 29), hw = bits(insn, 22, 21);
            if (opc == 1 || (!sf && hw > 1)) return UNSUPPORTED;
            u64 imm = static_cast<u64>(bits(insn, 20, 5)) << (hw * 16);
            if (opc == 0) setreg(c, rd_, ~imm, sf);
            else if (opc == 2) setreg(c, rd_, imm, sf);
            else setreg(c, rd_, (reg(c, rd_) & ~(0xFFFFull << (hw * 16))) | imm, sf);
            break;
        }
        case 6: {                                          // SBFM / BFM / UBFM
            u32 opc = bits(insn, 30, 29), n = bits(insn, 22, 22), immr = bits(insn, 21, 16), imms = bits(insn, 15, 10);
            u64 wmask, tmask;
            if (opc == 3 || n != static_cast<u32>(sf) || (!sf && ((immr | imms) & 0x20))) return UNSUPPORTED;
            if (!decode_bitmasks(n, imms, immr, false, sf, &wmask, &tmask)) return UNSUPPORTED;
            unsigned size = sf ? 64 : 32;
            u64 src = reg(c, rn);
            if (!sf) src = static_cast<u32>(src);
            u64 dst = opc == 1 ? reg(c, rd_) : 0;
            u64 bot = (dst & ~wmask) | (ror(src, immr, size) & wmask);
            u64 top = opc == 0 ? (((src >> imms) & 1) ? ~0ull : 0) : dst;
            setreg(c, rd_, (top & ~tmask) | (bot & tmask), sf);
            break;
        }
        case 7: {                                          // EXTR
            if (bits(insn, 30, 29) != 0 || bits(insn, 21, 21)) return UNSUPPORTED;
            u32 lsb = bits(insn, 15, 10), rm = bits(insn, 20, 16);
            u64 hi = reg(c, rn), lo = reg(c, rm), r;
            if (sf) r = lsb ? (lo >> lsb) | (hi << (64 - lsb)) : lo;
            else { if (lsb > 31) return UNSUPPORTED; r = lsb ? (static_cast<u32>(lo) >> lsb) | (static_cast<u32>(hi) << (32 - lsb)) : static_cast<u32>(lo); }
            setreg(c, rd_, r, sf);
            break;
        }
        default: return UNSUPPORTED;
        }
        c.pc += 4;
        return NEXT;
    }
    // ---------------------------------------------------------------- branches and system
    case 0xA: case 0xB: {
        if ((insn & 0x7C000000) == 0x14000000) {           // B / BL
            if (sf) c.x[30] = c.pc + 4;
            c.pc += sext(insn & 0x03FFFFFF, 26) * 4;
            return BRANCH;
        }
        if ((insn & 0x7E000000) == 0x34000000) {           // CBZ / CBNZ
            u64 v = reg(c, rd_);
            if (!sf) v = static_cast<u32>(v);
            bool take = (v == 0) != static_cast<bool>(insn & (1u << 24));
            c.pc += take ? sext(bits(insn, 23, 5), 19) * 4 : 4;
            return BRANCH;
        }
        if ((insn & 0x7E000000) == 0x36000000) {           // TBZ / TBNZ
            u32 bit = (bits(insn, 31, 31) << 5) | bits(insn, 23, 19);
            bool set = (reg(c, rd_) >> bit) & 1;
            bool take = set == static_cast<bool>(insn & (1u << 24));
            c.pc += take ? sext(bits(insn, 18, 5), 14) * 4 : 4;
            return BRANCH;
        }
        if ((insn & 0xFF000010) == 0x54000000) {           // B.cond
            c.pc += cond_holds(c, insn & 15) ? sext(bits(insn, 23, 5), 19) * 4 : 4;
            return BRANCH;
        }
        if ((insn & 0xFFFFFC1F) == 0xD61F0000) { c.pc = reg(c, rn); return BRANCH; }                                   // BR
        if ((insn & 0xFFFFFC1F) == 0xD63F0000) { u64 t = reg(c, rn); c.x[30] = c.pc + 4; c.pc = t; return BRANCH; }    // BLR
        if ((insn & 0xFFFFFC1F) == 0xD65F0000) { c.pc = reg(c, rn); return BRANCH; }                                   // RET
        if ((insn & 0xFFE0001F) == 0xD4000001) return SVC;
        if ((insn & 0xFFFFF01F) == 0xD503201F) { c.pc += 4; return NEXT; }                                              // NOP and hints
        if ((insn & 0xFFFFFFE0) == 0xD53BD040) { setreg(c, rd_, tpidr, true); c.pc += 4; return NEXT; }                 // MRS tpidr_el0
        return UNSUPPORTED;
    }
    // ---------------------------------------------------------------- loads and stores
    case 0x4: case 0x6: case 0xC: case 0xE: {
        const bool vreg = insn & (1u << 26);
        u32 op_hi = bits(insn, 29, 27);
        if (op_hi == 3 && !(insn & (1u << 24))) {          // load literal
            u32 opc = bits(insn, 31, 30);
            u64 addr = c.pc + sext(bits(insn, 23, 5), 19) * 4;
            if (vreg) {
                if (opc == 0) c.v[rd_] = {rd<u32>(addr), 0};
                else if (opc == 1) c.v[rd_] = {rd<u64>(addr), 0};
                else if (opc == 2) c.v[rd_] = {rd<u64>(addr), rd<u64>(addr + 8)};
                else return UNSUPPORTED;
            } else {
                if (opc == 0) setreg(c, rd_, rd<u32>(addr), true);
                else if (opc == 1) setreg(c, rd_, rd<u64>(addr), true);
                else if (opc == 2) setreg(c, rd_, static_cast<u64>(static_cast<i64>(rd<i32>(addr))), true);
                // opc 3 is a prefetch: nothing to do
            }
            c.pc += 4;
            return NEXT;
        }
        if (op_hi == 5) {                                   // load/store pair
            u32 opc = bits(insn, 31, 30), type = bits(insn, 24, 23), rt2 = bits(insn, 14, 10);
            bool load = insn & (1u << 22);
            unsigned size;                                 // bytes per register
            if (vreg) { if (opc == 3) return UNSUPPORTED; size = 4u << opc; }
            else { if (opc == 3 || (opc == 1 && !load)) return UNSUPPORTED; size = opc == 2 ? 8 : 4; }
            i64 off = sext(bits(insn, 21, 15), 7) * static_cast<i64>(size);
            u64 base = regsp(c, rn);
            u64 addr = type == 1 ? base : base + off;      // post-index uses the old base
            if (vreg) {
                for (int i = 0; i < 2; i++) {
                    u32 r = i ? rt2 : rd_;
                    u64 a = addr + static_cast<u64>(i) * size;
                    if (load) {
                        if (size == 4) c.v[r] = {rd<u32>(a), 0};
                        else if (size == 8) c.v[r] = {rd<u64>(a), 0};
                        else c.v[r] = {rd<u64>(a), rd<u64>(a + 8)};
                    } else {
                        if (size == 4) wr<u32>(a, static_cast<u32>(c.v[r][0]));
                        else if (size == 8) wr<u64>(a, c.v[r][0]);
                        else { wr<u64>(a, c.v[r][0]); wr<u64>(a + 8, c.v[r][1]); }
                    }
                }
            } else if (load) {
                u64 v1, v2;
                if (size == 8) { v1 = rd<u64>(addr); v2 = rd<u64>(addr + 8); }
                else if (opc == 1) { v1 = static_cast<u64>(static_cast<i64>(rd<i32>(addr))); v2 = static_cast<u64>(static_cast<i64>(rd<i32>(addr + 4))); }
                else { v1 = rd<u32>(addr); v2 = rd<u32>(addr + 4); }
                setreg(c, rd_, v1, true);
                setreg(c, rt2, v2, true);
            } else {
                if (size == 8) { wr<u64>(addr, reg(c, rd_)); wr<u64>(addr + 8, reg(c, rt2)); }
                else { wr<u32>(addr, static_cast<u32>(reg(c, rd_))); wr<u32>(addr + 4, static_cast<u32>(reg(c, rt2))); }
            }
            if (type == 1 || type == 3) { if (rn == 31) c.sp = base + off; else c.x[rn] = base + off; }
            c.pc += 4;
            return NEXT;
        }
        if (op_hi == 7) {                                   // single register forms
            u32 size = bits(insn, 31, 30), opc = bits(insn, 23, 22);
            u64 base = regsp(c, rn), addr;
            bool writeback = false;
            u64 wb_value = 0;
            if (insn & (1u << 24)) {                        // unsigned scaled offset
                unsigned scale = vreg && (opc & 2) ? 4 : size;
                addr = base + (static_cast<u64>(bits(insn, 21, 10)) << scale);
            } else if (!(insn & (1u << 21))) {              // 9-bit immediate: unscaled / post / pre
                u32 mode = bits(insn, 11, 10);
                i64 imm = sext(bits(insn, 20, 12), 9);
                if (mode == 2) return UNSUPPORTED;
                addr = mode == 1 ? base : base + imm;
                if (mode & 1) { writeback = true; wb_value = base + imm; }
            } else if (bits(insn, 11, 10) == 2) {           // register offset
                u32 option = bits(insn, 15, 13), rm = bits(insn, 20, 16);
                if (!(option & 2)) return UNSUPPORTED;
                unsigned scale = vreg && (opc & 2) ? 4 : size;
                unsigned shift = (insn & (1u << 12)) ? scale : 0;
                addr = base + extend_op(reg(c, rm), option, shift);
            } else {
                return UNSUPPORTED;                         // atomics, pointer auth
            }
            if (vreg) {
                if (opc & 2) {                              // 128-bit
                    if (size != 0) return UNSUPPORTED;
                    if (opc == 3) c.v[rd_] = {rd<u64>(addr), rd<u64>(addr + 8)};
                    else { wr<u64>(addr, c.v[rd_][0]); wr<u64>(addr + 8, c.v[rd_][1]); }
                } else if (opc == 1) {
                    c.v[rd_] = {size == 0 ? rd<u8>(addr) : size == 1 ? rd<u16>(addr) : size == 2 ? rd<u32>(addr) : rd<u64>(addr), 0};
                } else {
                    u64 v = c.v[rd_][0];
                    if (size == 0) wr<u8>(addr, static_cast<u8>(v)); else if (size == 1) wr<u16>(addr, static_cast<u16>(v));
                    else if (size == 2) wr<u32>(addr, static_cast<u32>(v)); else wr<u64>(addr, v);
                }
            } else if (opc == 0) {                          // store
                u64 v = reg(c, rd_);
                if (size == 0) wr<u8>(addr, static_cast<u8>(v)); else if (size == 1) wr<u16>(addr, static_cast<u16>(v));
                else if (size == 2) wr<u32>(addr, static_cast<u32>(v)); else wr<u64>(addr, v);
            } else if (opc == 1) {                          // load, zero-extend
                setreg(c, rd_, size == 0 ? rd<u8>(addr) : size == 1 ? rd<u16>(addr) : size == 2 ? rd<u32>(addr) : rd<u64>(addr), true);
            } else if (size == 3) {
                if (opc != 2) return UNSUPPORTED;           // opc 2 at size 3 is a prefetch: nothing to do
            } else {                                        // load, sign-extend (opc 2: to 64 bits, 3: to 32 bits)
                if (size == 2 && opc == 3) return UNSUPPORTED;
                i64 v = size == 0 ? rd<int8_t>(addr) : size == 1 ? rd<int16_t>(addr) : rd<i32>(addr);
                setreg(c, rd_, static_cast<u64>(v), opc == 2);
            }
            if (writeback) { if (rn == 31) c.sp = wb_value; else c.x[rn] = wb_value; }
            c.pc += 4;
            return NEXT;
        }
        return UNSUPPORTED;                                 // exclusives, SIMD structure loads
    }
    // ---------------------------------------------------------------- data processing, register
    case 0x5: case 0xD: {
        const u32 rm = bits(insn, 20, 16);
        if (!(insn & (1u << 28))) {
            if (!(insn & (1u << 24))) {                     // logical, shifted register
                u32 amount = bits(insn, 15, 10);
                if (!sf && amount > 31) return UNSUPPORTED;
                u64 b = shift_op(reg(c, rm), bits(insn, 23, 22), amount, sf);
                if (insn & (1u << 21)) b = ~b;
                u64 a = reg(c, rn), r;
                u32 opc = bits(insn, 30, 29);
                r = opc == 1 ? a | b : opc == 2 ? a ^ b : a & b;
                if (!sf) r = static_cast<u32>(r);
                if (opc == 3) { set_nz(c, r, sf); c.nzcv &= ~3u; }
                setreg(c, rd_, r, sf);
            } else {                                        // add/sub, shifted or extended register
                bool sub = insn & (1u << 30), s = insn & (1u << 29);
                u64 a, b;
                bool extended = insn & (1u << 21);
                if (extended) {
                    if (bits(insn, 23, 22) != 0) return UNSUPPORTED;
                    u32 sh = bits(insn, 12, 10);
                    if (sh > 4) return UNSUPPORTED;
                    a = regsp(c, rn);
                    b = extend_op(reg(c, rm), bits(insn, 15, 13), sh);
                } else {
                    u32 amount = bits(insn, 15, 10), type = bits(insn, 23, 22);
                    if (type == 3 || (!sf && amount > 31)) return UNSUPPORTED;
                    a = reg(c, rn);
                    b = shift_op(reg(c, rm), type, amount, sf);
                }
                u64 r = sub ? add_carry(&c, a, ~b, 1, sf, s) : add_carry(&c, a, b, 0, sf, s);
                if (extended && !s) setregsp(c, rd_, r, sf); else setreg(c, rd_, r, sf);
            }
            c.pc += 4;
            return NEXT;
        }
        if (insn & (1u << 24)) {                            // multiply-add family
            u32 op31 = bits(insn, 23, 21), ra = bits(insn, 14, 10);
            bool o0 = insn & (1u << 15);
            if (bits(insn, 30, 29) != 0) return UNSUPPORTED;
            u64 a = reg(c, rn), b = reg(c, rm), acc = reg(c, ra), r;
            if (op31 == 0) { u64 p = a * b; r = o0 ? acc - p : acc + p; setreg(c, rd_, r, sf); }
            else if (!sf) return UNSUPPORTED;
            else if (op31 == 1) { u64 p = static_cast<u64>(static_cast<i64>(static_cast<i32>(a)) * static_cast<i64>(static_cast<i32>(b))); setreg(c, rd_, o0 ? acc - p : acc + p, true); }
            else if (op31 == 5) { u64 p = static_cast<u64>(static_cast<u32>(a)) * static_cast<u32>(b); setreg(c, rd_, o0 ? acc - p : acc + p, true); }
            else if (op31 == 2 && !o0) { i64 hi; _mul128(static_cast<i64>(a), static_cast<i64>(b), &hi); setreg(c, rd_, static_cast<u64>(hi), true); }
            else if (op31 == 6 && !o0) { u64 hi; _umul128(a, b, &hi); setreg(c, rd_, hi, true); }
            else return UNSUPPORTED;
            c.pc += 4;
            return NEXT;
        }
        switch (bits(insn, 24, 21)) {
        case 0: {                                           // ADC / SBC
            if (bits(insn, 15, 10) != 0) return UNSUPPORTED;
            bool sub = insn & (1u << 30), s = insn & (1u << 29);
            u64 b = reg(c, rm);
            setreg(c, rd_, add_carry(&c, reg(c, rn), sub ? ~b : b, (c.nzcv >> 1) & 1, sf, s), sf);
            break;
        }
        case 2: {                                           // CCMN / CCMP
            if (!(insn & (1u << 29)) || (insn & (1u << 10)) || (insn & (1u << 4))) return UNSUPPORTED;
            if (cond_holds(c, bits(insn, 15, 12))) {
                u64 b = (insn & (1u << 11)) ? rm : reg(c, rm);
                if (insn & (1u << 30)) add_carry(&c, reg(c, rn), ~b, 1, sf, true);
                else add_carry(&c, reg(c, rn), b, 0, sf, true);
            } else {
                c.nzcv = insn & 15;
            }
            break;
        }
        case 4: {                                           // CSEL / CSINC / CSINV / CSNEG
            if ((insn & (1u << 29)) || (insn & (1u << 11))) return UNSUPPORTED;
            u64 r;
            if (cond_holds(c, bits(insn, 15, 12))) r = reg(c, rn);
            else {
                r = reg(c, rm);
                bool inv = insn & (1u << 30), inc = insn & (1u << 10);
                if (inv) r = ~r;
                if (inc) r += 1;
            }
            setreg(c, rd_, r, sf);
            break;
        }
        case 6: {
            u32 opcode = bits(insn, 15, 10);
            if (insn & (1u << 29)) return UNSUPPORTED;
            if (!(insn & (1u << 30))) {                     // two-source
                u64 a = reg(c, rn), b = reg(c, rm), r;
                unsigned size = sf ? 64 : 32;
                if (!sf) { a = static_cast<u32>(a); b = static_cast<u32>(b); }
                switch (opcode) {
                case 2: r = b ? a / b : 0; break;
                case 3:
                    if (sf) { i64 sa = static_cast<i64>(a), sb = static_cast<i64>(b); r = !sb ? 0 : (sa == INT64_MIN && sb == -1) ? static_cast<u64>(sa) : static_cast<u64>(sa / sb); }
                    else { i32 sa = static_cast<i32>(a), sb = static_cast<i32>(b); r = !sb ? 0 : (sa == INT32_MIN && sb == -1) ? static_cast<u32>(sa) : static_cast<u32>(sa / sb); }
                    break;
                case 8: r = shift_op(a, 0, static_cast<u32>(b % size), sf); break;
                case 9: r = shift_op(a, 1, static_cast<u32>(b % size), sf); break;
                case 10: r = shift_op(a, 2, static_cast<u32>(b % size), sf); break;
                case 11: r = shift_op(a, 3, static_cast<u32>(b % size), sf); break;
                default: return UNSUPPORTED;
                }
                setreg(c, rd_, r, sf);
            } else {                                        // one-source
                if (rm != 0) return UNSUPPORTED;
                u64 a = reg(c, rn), r;
                if (!sf) a = static_cast<u32>(a);
                switch (opcode) {
                case 0: {                                   // RBIT
                    r = 0;
                    unsigned size = sf ? 64 : 32;
                    for (unsigned i = 0; i < size; i++) if (a & (1ull << i)) r |= 1ull << (size - 1 - i);
                    break;
                }
                case 1: r = sf ? ((a & 0xFF00FF00FF00FF00ull) >> 8) | ((a & 0x00FF00FF00FF00FFull) << 8)
                               : ((a & 0xFF00FF00ull) >> 8) | ((a & 0x00FF00FFull) << 8); break;           // REV16
                case 2: r = sf ? (static_cast<u64>(_byteswap_ulong(static_cast<u32>(a >> 32))) << 32) | _byteswap_ulong(static_cast<u32>(a))
                               : _byteswap_ulong(static_cast<u32>(a)); break;                               // REV32 / REV (32-bit)
                case 3: if (!sf) return UNSUPPORTED; r = _byteswap_uint64(a); break;                         // REV
                case 4: r = sf ? (a ? __lzcnt64(a) : 64) : (a ? __lzcnt(static_cast<u32>(a)) : 32); break;   // CLZ
                default: return UNSUPPORTED;
                }
                setreg(c, rd_, r, sf);
            }
            break;
        }
        default: return UNSUPPORTED;
        }
        c.pc += 4;
        return NEXT;
    }
    // ---------------------------------------------------------------- scalar floating point (a small, common subset)
    case 0x7: case 0xF: {
        if ((insn & 0x5F200000) != 0x1E200000) {
            // MOVI Vd, #0 (all element sizes): the usual way to zero a vector register.
            if ((insn & 0x9FF80C00) == 0x0F000400 && bits(insn, 18, 16) == 0 && bits(insn, 9, 5) == 0) {
                u32 cmode = bits(insn, 15, 12);
                bool op = insn & (1u << 29);
                if ((!op && (cmode == 0xE || (cmode & 9) == 0 || (cmode & 0xD) == 8)) || (op && cmode == 0xE)) {
                    c.v[rd_] = {0, 0};
                    c.pc += 4;
                    return NEXT;
                }
            }
            return UNSUPPORTED;
        }
        u32 type = bits(insn, 23, 22);
        if (type > 1 || (insn & (1u << 29))) return UNSUPPORTED;
        const bool dbl = type == 1;
        const u32 rm = bits(insn, 20, 16);
        if (bits(insn, 11, 10) == 2) {                      // two-source arithmetic
            u32 opcode = bits(insn, 15, 12);
            if (opcode > 3 || sf) return UNSUPPORTED;
            // NaN results follow ARM's rules (FPProcessNaNs), not x86's: a signalling
            // NaN operand wins, then a quiet one, else the positive default NaN.
            if (dbl) {
                double a = vd(c, rn), b = vd(c, rm);
                double r = opcode == 0 ? a * b : opcode == 1 ? a / b : opcode == 2 ? a + b : a - b;
                if (r != r) {
                    u64 ua = c.v[rn][0], ub = c.v[rm][0];
                    const u64 quiet = 1ull << 51;
                    bool na = a != a, nb = b != b;
                    u64 out = na && !(ua & quiet) ? ua | quiet : nb && !(ub & quiet) ? ub | quiet : na ? ua : nb ? ub : 0x7FF8000000000000ull;
                    c.v[rd_] = {out, 0};
                } else {
                    set_vd(c, rd_, r);
                }
            } else {
                float a = vs(c, rn), b = vs(c, rm);
                float r = opcode == 0 ? a * b : opcode == 1 ? a / b : opcode == 2 ? a + b : a - b;
                if (r != r) {
                    u32 ua = static_cast<u32>(c.v[rn][0]), ub = static_cast<u32>(c.v[rm][0]);
                    const u32 quiet = 1u << 22;
                    bool na = a != a, nb = b != b;
                    u32 out = na && !(ua & quiet) ? ua | quiet : nb && !(ub & quiet) ? ub | quiet : na ? ua : nb ? ub : 0x7FC00000u;
                    c.v[rd_] = {out, 0};
                } else {
                    set_vs(c, rd_, r);
                }
            }
        } else if (bits(insn, 14, 10) == 0x10 && !sf) {     // one-source: FMOV / FABS / FNEG / FSQRT / FCVT
            u32 opcode = bits(insn, 20, 15);
            if (opcode == 0) { if (dbl) c.v[rd_] = {c.v[rn][0], 0}; else c.v[rd_] = {static_cast<u32>(c.v[rn][0]), 0}; }
            else if (opcode == 1) { if (dbl) set_vd(c, rd_, std::fabs(vd(c, rn))); else set_vs(c, rd_, std::fabs(vs(c, rn))); }
            else if (opcode == 2) { if (dbl) c.v[rd_] = {c.v[rn][0] ^ (1ull << 63), 0}; else c.v[rd_] = {static_cast<u32>(c.v[rn][0]) ^ 0x80000000u, 0}; }
            else if (opcode == 4 && dbl) set_vs(c, rd_, static_cast<float>(vd(c, rn)));
            else if (opcode == 5 && !dbl) set_vd(c, rd_, static_cast<double>(vs(c, rn)));
            else return UNSUPPORTED;
        } else if (bits(insn, 13, 10) == 8 && !sf && bits(insn, 15, 14) == 0) {   // FCMP / FCMPE
            u32 op2 = insn & 31;
            if (op2 & 7) return UNSUPPORTED;
            double a = dbl ? vd(c, rn) : vs(c, rn);
            double b = (op2 & 8) ? 0.0 : (dbl ? vd(c, rm) : vs(c, rm));
            c.nzcv = (a != a || b != b) ? 3 : a == b ? 6 : a < b ? 8 : 2;
        } else if (bits(insn, 12, 10) == 4 && !sf) {        // FMOV immediate
            if (bits(insn, 9, 5) != 0) return UNSUPPORTED;
            u32 imm8 = bits(insn, 20, 13);
            u64 sign = imm8 >> 7, b6 = (imm8 >> 6) & 1, frac = imm8 & 0x3F;
            if (dbl) c.v[rd_] = {(sign << 63) | ((b6 ? 0x3FCull : 0x400ull) << 52) | (frac << 48), 0};
            else c.v[rd_] = {(sign << 31) | ((b6 ? 0x7Cull : 0x80ull) << 23) | (frac << 19), 0};
        } else if (bits(insn, 15, 10) == 0) {               // conversions between integer and FP registers
            u32 rmode = bits(insn, 20, 19), opcode = bits(insn, 18, 16);
            if (rmode == 0 && opcode == 2) {                // SCVTF
                i64 v = sf ? static_cast<i64>(reg(c, rn)) : static_cast<i32>(reg(c, rn));
                if (dbl) set_vd(c, rd_, static_cast<double>(v)); else set_vs(c, rd_, static_cast<float>(v));
            } else if (rmode == 0 && opcode == 3) {         // UCVTF
                u64 v = sf ? reg(c, rn) : static_cast<u32>(reg(c, rn));
                if (dbl) set_vd(c, rd_, static_cast<double>(v)); else set_vs(c, rd_, static_cast<float>(v));
            } else if (rmode == 3 && opcode == 0) {         // FCVTZS (saturating)
                double v = dbl ? vd(c, rn) : vs(c, rn);
                u64 r;
                if (v != v) r = 0;
                else if (sf) r = v >= 9223372036854775808.0 ? INT64_MAX : v <= -9223372036854775808.0 ? static_cast<u64>(INT64_MIN) : static_cast<u64>(static_cast<i64>(v));
                else r = v >= 2147483648.0 ? INT32_MAX : v <= -2147483649.0 ? static_cast<u32>(INT32_MIN) : static_cast<u32>(static_cast<i32>(v));
                setreg(c, rd_, r, sf);
            } else if (rmode == 0 && opcode == 6 && sf == dbl) {   // FMOV: FP register -> general register
                setreg(c, rd_, dbl ? c.v[rn][0] : static_cast<u32>(c.v[rn][0]), sf);
            } else if (rmode == 0 && opcode == 7 && sf == dbl) {   // FMOV: general register -> FP register
                c.v[rd_] = {dbl ? reg(c, rn) : static_cast<u32>(reg(c, rn)), 0};
            } else {
                return UNSUPPORTED;
            }
        } else {
            return UNSUPPORTED;
        }
        c.pc += 4;
        return NEXT;
    }
    default:
        return UNSUPPORTED;
    }
}
}  // namespace

interp::Stop interp::run(Cpu& c, u64 tpidr, u32* svc, u64* executed) {
    u64 count = 0;
    for (;;) {
        if (enter_block(c.pc)) { *executed += count; return Stop::WantJit; }
        for (;;) {
            u32 insn = rd<u32>(c.pc);
            int r;
            if (verify) {
                Cpu before = c;
                r = exec(c, insn, tpidr);
                if (r == NEXT || r == BRANCH) verify(before, c, insn);
            } else {
                r = exec(c, insn, tpidr);
            }
            count++;
            if (r == NEXT) continue;
            if (r == BRANCH) break;
            if (r == SVC) {
                *svc = bits(insn, 20, 5);
                c.pc += 4;
                *executed += count;
                return Stop::Svc;
            }
            // Unsupported: translate from here on, and remember so we come straight to the JIT next time.
            if (g_stats) {
                std::lock_guard lk(g_unsupported_mutex);
                auto& e = g_unsupported[insn & 0xFFE0FC00];
                e.first++;
                e.second = insn;
            }
            mark_hot(c.pc);
            *executed += count - 1;
            return Stop::WantJit;
        }
    }
}

#pragma once
// Included by every generated translation shard.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <limits>

#include "recomp/cpu.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/native.hpp"
#include "recomp/x86ops.hpp"

namespace voy::recomp {

// A guest fault (int3, ud2, divide error, a call that should never return):
// SEH is not emulated for these, so the game stops with a report.
[[noreturn]] void trap(Cpu& c, std::uint32_t va, const char* what);
// A guest function the translator could not translate, or did not select.
[[noreturn]] void guest_unported(Cpu& c, std::uint32_t va, const char* why);
void guest_cpuid(Cpu& c);
// `pause` in a spin loop: the other guest threads get a turn.
void guest_pause(Cpu& c);
// Set every couple of milliseconds while another guest thread could run; the
// translated code checks it at loop back-edges and yields (GUEST_PREEMPT).
extern std::atomic<bool> g_preempt;
// Counters a temporary translator probe can bump (printed with DCUSA_WATCH).
extern std::uint64_t g_probe[8];
void debug_probe(Cpu& c, std::uint32_t va, std::uint32_t a, std::uint32_t b, std::uint32_t d, std::uint32_t e);
void guest_preempt(Cpu& c);
// Debugging aid (DCUSA_NAN_TRAP in tools/x86tr/x87.py): reports the first NaN
// an x87 store at `site` writes, once per site.
void nan_trap_report(Cpu& c, std::uint32_t site);
inline void nan_trap(Cpu& c, std::uint32_t site, double v) {
    if (v != v) nan_trap_report(c, site);
}
std::uint64_t guest_rdtsc();

template <class T, int N, class V, class Op> inline void lanes(V& d, const V& s, Op op) {
    T* dp = reinterpret_cast<T*>(&d);
    const T* sp = reinterpret_cast<const T*>(&s);
    for (int i = 0; i < N; ++i) {
        T a, b;
        std::memcpy(&a, dp + i, sizeof(T));
        std::memcpy(&b, sp + i, sizeof(T));
        T r = op(a, b);
        std::memcpy(dp + i, &r, sizeof(T));
    }
}

// NaN results the x86 way. An invalid operation on non-NaN inputs gives the
// "default NaN", which is negative on x86 (FFC00000) and positive on ARM; with
// NaN inputs x86 returns the first NaN operand (quieted) where ARM prefers a
// signalling NaN wherever it is.
template <class T> inline T x86_quiet(T v) {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(std::bit_cast<std::uint32_t>(v) | 0x00400000u);
    else return std::bit_cast<T>(std::bit_cast<std::uint64_t>(v) | 0x0008000000000000ull);
}
template <class T> inline T x86_default_nan() {
    if constexpr (sizeof(T) == 4) return std::bit_cast<T>(0xFFC00000u);
    else return std::bit_cast<T>(0xFFF8000000000000ull);
}
template <class T> inline T x86_nan(T r, T a, T b) {
    if (r == r) return r;
    if (a != a) return x86_quiet(a);
    if (b != b) return x86_quiet(b);
    return x86_default_nan<T>();
}
template <class T> inline T x86_nan(T r, T a) {
    if (r == r) return r;
    if (a != a) return x86_quiet(a);
    return x86_default_nan<T>();
}

// Locked x86 operations and xchg with memory. Sequentially consistent, which is
// what the lock prefix guarantees; the address must be naturally aligned (the
// translator traps otherwise, since a split lock has no single host instruction).
template <class T> inline T atomic_load(std::uint8_t* M, std::uint32_t a) {
    return __atomic_load_n(reinterpret_cast<T*>(M + a), __ATOMIC_SEQ_CST);
}
template <class T> inline T atomic_xchg(std::uint8_t* M, std::uint32_t a, T v) {
    return __atomic_exchange_n(reinterpret_cast<T*>(M + a), v, __ATOMIC_SEQ_CST);
}
template <class T> inline bool atomic_cmpxchg(std::uint8_t* M, std::uint32_t a, T& expected, T desired) {
    return __atomic_compare_exchange_n(reinterpret_cast<T*>(M + a), &expected, desired, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
template <class T, class Op> inline T atomic_rmw(std::uint8_t* M, std::uint32_t a, Op op) {
    auto* p = reinterpret_cast<T*>(M + a);
    T seen = __atomic_load_n(p, __ATOMIC_SEQ_CST);
    for (;;) {
        if (__atomic_compare_exchange_n(p, &seen, op(seen), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return seen;
    }
}
inline void fence_full() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
inline void fence_store() { __atomic_thread_fence(__ATOMIC_RELEASE); }
inline void fence_load() { __atomic_thread_fence(__ATOMIC_ACQUIRE); }

inline Xmm xmm_load(const std::uint8_t* M, std::uint32_t a, unsigned bytes) {
    Xmm v{};
    std::memcpy(&v, M + a, bytes);
    return v;
}
inline Mmx mmx_load(const std::uint8_t* M, std::uint32_t a, unsigned bytes) {
    Mmx v{};
    std::memcpy(&v, M + a, bytes);
    return v;
}
inline Xmm xmm_from(const Mmx& m) {
    Xmm v{};
    std::memcpy(&v, &m, 8);
    return v;
}

// 80-bit extended values in guest memory (fld / fstp tbyte), converted to and from double.
inline double ld80(const std::uint8_t* M, std::uint32_t a) {
    std::uint64_t mant;
    std::uint16_t se;
    std::memcpy(&mant, M + a, 8);
    std::memcpy(&se, M + a + 8, 2);
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0x7FFF) v = (mant << 1) ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else if (mant == 0) v = 0.0;
    else v = std::ldexp(double(mant), exp - 16383 - 63);
    return (se & 0x8000) ? -v : v;
}
inline void st80(std::uint8_t* M, std::uint32_t a, double v) {
    std::uint64_t mant = 0;
    std::uint16_t se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) { mant = 0xC000000000000000ull; se |= 0x7FFF; }
    else if (std::isinf(v)) { mant = 0x8000000000000000ull; se |= 0x7FFF; }
    else if (v != 0.0) {
        int e;
        double f = std::frexp(std::fabs(v), &e);
        mant = std::uint64_t(std::ldexp(f, 64));
        se |= std::uint16_t(e - 1 + 16383);
    }
    std::memcpy(M + a, &mant, 8);
    std::memcpy(M + a + 8, &se, 2);
}
// fxam: class of ST(0) into C3 / C2 / C0, its sign into C1.
inline void fpu_examine(Fpu& f) {
    double v = f.at(0);
    std::uint16_t s = std::uint16_t(f.status & ~0x4700u);
    if (std::signbit(v)) s |= 0x0200;
    if (((f.tag >> (2 * f.top)) & 3) == 3) s |= 0x4100;
    else if (std::isnan(v)) s |= 0x0100;
    else if (std::isinf(v)) s |= 0x0500;
    else if (v == 0.0) s |= 0x4000;
    else if (std::fpclassify(v) == FP_SUBNORMAL) s |= 0x4400;
    else s |= 0x0400;
    f.status = s;
}
inline double fpu_scale(double x, double y) {
    double n = std::trunc(y);
    if (n != n) return n;
    int e = n > 100000.0 ? 100000 : n < -100000.0 ? -100000 : int(n);
    return std::ldexp(x, e);
}
// fprem / fprem1: one complete reduction (C2 = 0), low quotient bits in C0 / C3 / C1.
inline void fpu_prem(Fpu& f, bool ieee) {
    double a = f.at(0), b = f.at(1);
    double q = ieee ? std::nearbyint(a / b) : std::trunc(a / b);
    double r = ieee ? std::remainder(a, b) : std::fmod(a, b);
    unsigned qi = std::isfinite(q) ? unsigned(std::fmod(std::fabs(q), 8.0)) : 0;
    std::uint16_t s = std::uint16_t(f.status & ~0x4700u);
    if (qi & 4) s |= 0x0100;
    if (qi & 2) s |= 0x4000;
    if (qi & 1) s |= 0x0200;
    f.status = s;
    f.at(0) = r;
}
// fnstenv / fnsave and fldenv / frstor, 32-bit protected-mode layout: the
// 28-byte environment (control, status, tag, then zeroed instruction and
// operand pointers), and for the full state the eight registers in ST order,
// 10 bytes each. fnsave then reinitialises the FPU like fninit.
inline void fpu_store_env(std::uint8_t* M, std::uint32_t a, const Fpu& f) {
    const std::uint32_t env[7] = {0xFFFF0000u | f.control, 0xFFFF0000u | f.status_word(), 0xFFFF0000u | f.tag, 0, 0, 0,
                                  0xFFFF0000u};
    std::memcpy(M + a, env, sizeof env);
}
inline void fpu_load_env(const std::uint8_t* M, std::uint32_t a, Fpu& f) {
    std::uint16_t control, status, tag;
    std::memcpy(&control, M + a, 2);
    std::memcpy(&status, M + a + 4, 2);
    std::memcpy(&tag, M + a + 8, 2);
    f.control = control;
    f.top = (status >> 11) & 7;
    f.status = std::uint16_t(status & ~0x3800u);
    f.tag = tag;
}
inline void fpu_save(std::uint8_t* M, std::uint32_t a, Fpu& f) {
    fpu_store_env(M, a, f);
    for (unsigned i = 0; i < 8; ++i) st80(M, a + 28 + 10 * i, f.at(i));
    f = Fpu{};
}
inline void fpu_restore(const std::uint8_t* M, std::uint32_t a, Fpu& f) {
    fpu_load_env(M, a, f);
    for (unsigned i = 0; i < 8; ++i) f.at(i) = ld80(M, a + 28 + 10 * i);
}
inline std::int16_t fpu_trunc_i16(double v) {
    double r = std::trunc(v);
    if (!(r >= -32768.0 && r <= 32767.0)) return std::numeric_limits<std::int16_t>::min();
    return std::int16_t(r);
}

inline float f32_bits(std::uint32_t v) { return std::bit_cast<float>(v); }
inline std::uint32_t bits_f32(float v) { return std::bit_cast<std::uint32_t>(v); }
inline double f64_bits(std::uint64_t v) { return std::bit_cast<double>(v); }
inline std::uint64_t bits_f64(double v) { return std::bit_cast<std::uint64_t>(v); }

}

using namespace voy::recomp;

// Registers live in locals for the length of a translated function and are
// written back before anything that can observe them (calls, returns, traps).
#define PROLOGUE()                                                                                   \
    std::uint8_t* const M = c.mem;                                                                   \
    std::uint32_t eax = c.r[0], ecx = c.r[1], edx = c.r[2], ebx = c.r[3], esp = c.r[4], ebp = c.r[5], \
                  esi = c.r[6], edi = c.r[7];                                                        \
    std::uint8_t cf = c.cf, pf = c.pf, af = c.af, zf = c.zf, sf = c.sf, of = c.of;                   \
    std::uint32_t ta = 0, tb = 0;                                                                    \
    (void)M; (void)ta; (void)tb
// PROLOGUE for a function that reads no flags before writing them.
#define PROLOGUE_NF()                                                                                \
    std::uint8_t* const M = c.mem;                                                                   \
    std::uint32_t eax = c.r[0], ecx = c.r[1], edx = c.r[2], ebx = c.r[3], esp = c.r[4], ebp = c.r[5], \
                  esi = c.r[6], edi = c.r[7];                                                        \
    std::uint8_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0;                                     \
    std::uint32_t ta = 0, tb = 0;                                                                    \
    (void)M; (void)ta; (void)tb; (void)cf; (void)pf; (void)af; (void)zf; (void)sf; (void)of
// The write-back before a direct call. translate.py replaces each with SYNC_R
// or SYNC; left unresolved (the semantics tests) it is the full SYNC.
#define SYNC_TO(target) SYNC()
// Single-threaded guests: nothing to yield to at loop back-edges.
#define GUEST_PREEMPT() do { } while (0)
#define SYNC()                                                                                       \
    (c.r[0] = eax, c.r[1] = ecx, c.r[2] = edx, c.r[3] = ebx, c.r[4] = esp, c.r[5] = ebp, c.r[6] = esi, \
     c.r[7] = edi, c.cf = cf, c.pf = pf, c.af = af, c.zf = zf, c.sf = sf, c.of = of)
// SYNC without the flags: a ret whose flags no caller reads (translate.py
// find_flag_returning decides which do).
#define SYNC_R()                                                                                     \
    (c.r[0] = eax, c.r[1] = ecx, c.r[2] = edx, c.r[3] = ebx, c.r[4] = esp, c.r[5] = ebp, c.r[6] = esi, \
     c.r[7] = edi)
// The read-back after a call to a callee that returns some registers
// unchanged (translate.py preserved_regs): eax, ecx, edx and esp always, then
// by `m`: 1 ebx, 2 ebp, 4 esi, 8 edi, 16 the flags. RELOAD() is RELOAD_M(31).
#define RELOAD_M(m)                                                                                  \
    (eax = c.r[0], ecx = c.r[1], edx = c.r[2], esp = c.r[4], ((m) & 1 ? (void)(ebx = c.r[3]) : (void)0), \
     ((m) & 2 ? (void)(ebp = c.r[5]) : (void)0), ((m) & 4 ? (void)(esi = c.r[6]) : (void)0),          \
     ((m) & 8 ? (void)(edi = c.r[7]) : (void)0),                                                       \
     ((m) & 16 ? (void)(cf = c.cf, pf = c.pf, af = c.af, zf = c.zf, sf = c.sf, of = c.of) : (void)0))
// Left unresolved (the semantics tests): everything.
#define RELOAD_AFTER(target, flags) RELOAD()
#define RELOAD()                                                                                     \
    (eax = c.r[0], ecx = c.r[1], edx = c.r[2], ebx = c.r[3], esp = c.r[4], ebp = c.r[5], esi = c.r[6], \
     edi = c.r[7], cf = c.cf, pf = c.pf, af = c.af, zf = c.zf, sf = c.sf, of = c.of)

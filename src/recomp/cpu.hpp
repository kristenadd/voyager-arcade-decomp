#pragma once
// Guest machine state for translated x86-32 code.
#include <cstdint>
#include <cstring>

namespace voy::recomp {

union Xmm {
    float f[4];
    double d[2];
    std::uint8_t u8[16];
    std::int8_t i8[16];
    std::uint16_t u16[8];
    std::int16_t i16[8];
    std::uint32_t u32[4];
    std::int32_t i32[4];
    std::uint64_t u64[2];
    std::int64_t i64[2];
};
static_assert(sizeof(Xmm) == 16);

union Mmx {
    std::uint8_t u8[8];
    std::int8_t i8[8];
    std::uint16_t u16[4];
    std::int16_t i16[4];
    std::uint32_t u32[2];
    std::int32_t i32[2];
    std::uint64_t u64;
    std::int64_t i64;
};

// x87 registers hold doubles. Linux runs x87 at 64-bit precision (PC_64); the
// game's arithmetic is float and double, so doubles differ only in the last
// bits of intermediate results kept on the FPU stack.
struct Fpu {
    double st[8] = {};
    unsigned top = 0;              // physical index of ST(0)
    std::uint16_t control = 0x037F;  // Linux default: PC_64, round to nearest, all exceptions masked
    std::uint16_t status = 0;      // condition codes C0..C3 (top is kept in `top`)
    std::uint16_t tag = 0xFFFF;    // 2 bits per register; 3 = empty

    double& at(unsigned i) { return st[(top + i) & 7]; }
    void push(double v) {
        top = (top - 1) & 7;
        st[top] = v;
        tag &= std::uint16_t(~(3u << (2 * top)));
    }
    double pop() {
        double v = st[top];
        tag |= std::uint16_t(3u << (2 * top));
        top = (top + 1) & 7;
        return v;
    }
    std::uint16_t status_word() const { return std::uint16_t((status & ~0x3800u) | (top << 11)); }
};

struct Cpu {
    // eax, ecx, edx, ebx, esp, ebp, esi, edi: translated functions keep these
    // in locals and write them back around calls.
    std::uint32_t r[8] = {};
    // Arithmetic flags, one byte each (0 / 1); the direction flag as well.
    std::uint8_t cf = 0, pf = 0, af = 0, zf = 0, sf = 0, of = 0, df = 0;
    // EFLAGS.AC and EFLAGS.ID as last written by popfd (CPU feature probes toggle them).
    std::uint32_t eflags_extra = 0;
    Xmm xmm[8] = {};
    std::uint32_t mxcsr = 0x1F80;
    Fpu fpu;
    Mmx mm[8] = {};
    std::uint32_t fs_base = 0;   // guest address of this thread's TEB
    std::uint8_t* mem = nullptr;  // host address of guest address 0
    void* process = nullptr;      // the host Process this CPU runs (host/process.hpp)

    enum Reg { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

    std::uint32_t eflags() const {
        return 0x202u | std::uint32_t(cf) | std::uint32_t(pf) << 2 | std::uint32_t(af) << 4 | std::uint32_t(zf) << 6 |
               std::uint32_t(sf) << 7 | std::uint32_t(df) << 10 | std::uint32_t(of) << 11 | eflags_extra;
    }
    void set_eflags(std::uint32_t v) {
        cf = v & 1; pf = (v >> 2) & 1; af = (v >> 4) & 1; zf = (v >> 6) & 1;
        sf = (v >> 7) & 1; df = (v >> 10) & 1; of = (v >> 11) & 1;
        eflags_extra = v & 0x240000u;
    }
};

}

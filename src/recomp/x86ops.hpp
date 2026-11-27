#pragma once
// Helpers the translated code calls: guest memory access and the x86
// semantics that C++ / ARM64 do not give directly.
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "recomp/cpu.hpp"

namespace voy::recomp {

// ---- guest memory (M = host address of guest 0; addresses wrap at 4 GB) ----
inline std::uint8_t ld8(const std::uint8_t* M, std::uint32_t a) { return M[a]; }
inline std::uint16_t ld16(const std::uint8_t* M, std::uint32_t a) { std::uint16_t v; std::memcpy(&v, M + a, 2); return v; }
inline std::uint32_t ld32(const std::uint8_t* M, std::uint32_t a) { std::uint32_t v; std::memcpy(&v, M + a, 4); return v; }
inline std::uint64_t ld64(const std::uint8_t* M, std::uint32_t a) { std::uint64_t v; std::memcpy(&v, M + a, 8); return v; }
inline float ldf32(const std::uint8_t* M, std::uint32_t a) { float v; std::memcpy(&v, M + a, 4); return v; }
inline double ldf64(const std::uint8_t* M, std::uint32_t a) { double v; std::memcpy(&v, M + a, 8); return v; }
inline Xmm ld128(const std::uint8_t* M, std::uint32_t a) { Xmm v; std::memcpy(&v, M + a, 16); return v; }
inline void st8(std::uint8_t* M, std::uint32_t a, std::uint8_t v) { M[a] = v; }
inline void st16(std::uint8_t* M, std::uint32_t a, std::uint16_t v) { std::memcpy(M + a, &v, 2); }
inline void st32(std::uint8_t* M, std::uint32_t a, std::uint32_t v) { std::memcpy(M + a, &v, 4); }
inline void st64(std::uint8_t* M, std::uint32_t a, std::uint64_t v) { std::memcpy(M + a, &v, 8); }
inline void stf32(std::uint8_t* M, std::uint32_t a, float v) { std::memcpy(M + a, &v, 4); }
inline void stf64(std::uint8_t* M, std::uint32_t a, double v) { std::memcpy(M + a, &v, 8); }
inline void st128(std::uint8_t* M, std::uint32_t a, const Xmm& v) { std::memcpy(M + a, &v, 16); }

// ---- flags ----
inline std::uint8_t parity(std::uint32_t v) { return std::uint8_t(~std::popcount(v & 0xFFu) & 1); }

// ---- integer conversions: x86 answers the "integer indefinite" value when
// the result does not fit (ARM saturates; C++ calls it undefined) ----
inline std::int32_t cvt_trunc_i32(double v) {
    if (!(v > -2147483649.0 && v < 2147483648.0)) return std::numeric_limits<std::int32_t>::min();
    return std::int32_t(v);
}
inline std::int32_t cvt_round_i32(double v) {  // MXCSR / x87 round to nearest even
    double r = std::nearbyint(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return std::numeric_limits<std::int32_t>::min();
    return std::int32_t(r);
}
inline std::int64_t cvt_trunc_i64(double v) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(v);
}
inline std::int64_t cvt_round_i64(double v) {
    double r = std::nearbyint(v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(r);
}
inline std::int16_t cvt_round_i16(double v) {
    double r = std::nearbyint(v);
    if (!(r >= -32768.0 && r <= 32767.0)) return std::numeric_limits<std::int16_t>::min();
    return std::int16_t(r);
}

// ---- SSE min / max: the second operand when either is NaN or both are zero ----
inline float sse_min(float a, float b) { return a < b ? a : b; }
inline float sse_max(float a, float b) { return a > b ? a : b; }
inline double sse_min(double a, double b) { return a < b ? a : b; }
inline double sse_max(double a, double b) { return a > b ? a : b; }

// ---- SSE compare predicates (cmpps / cmpss immediate 0..7) ----
template <class T> inline bool sse_cmp(T a, T b, int pred) {
    switch (pred & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return a == a && b == b;
    }
}

// ---- saturation ----
inline std::int8_t sat_i8(int v) { return std::int8_t(v < -128 ? -128 : v > 127 ? 127 : v); }
inline std::uint8_t sat_u8(int v) { return std::uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }
inline std::int16_t sat_i16(int v) { return std::int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
inline std::uint16_t sat_u16(int v) { return std::uint16_t(v < 0 ? 0 : v > 65535 ? 65535 : v); }

// ---- comiss / ucomiss / fcomi style result into ZF, PF, CF ----
template <class T> inline void fp_compare(T a, T b, std::uint8_t& zf, std::uint8_t& pf, std::uint8_t& cf) {
    if (a != a || b != b) { zf = pf = cf = 1; return; }
    zf = a == b; pf = 0; cf = a < b;
}
// fcom / fcomp / fucom: C0 / C2 / C3 in the status word
inline void fpu_compare(Fpu& f, double a, double b) {
    std::uint16_t s = f.status & std::uint16_t(~0x4500u);
    if (a != a || b != b) s |= 0x4500;
    else if (a < b) s |= 0x0100;
    else if (a == b) s |= 0x4000;
    f.status = s;
}

// ---- x87 rounding by the control word's RC field ----
inline double fpu_round(const Fpu& f, double v) {
    switch ((f.control >> 10) & 3) {
    case 0: return std::nearbyint(v);
    case 1: return std::floor(v);
    case 2: return std::ceil(v);
    default: return std::trunc(v);
    }
}
inline std::int32_t fpu_to_i32(const Fpu& f, double v) {
    double r = fpu_round(f, v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return std::numeric_limits<std::int32_t>::min();
    return std::int32_t(r);
}
inline std::int16_t fpu_to_i16(const Fpu& f, double v) {
    double r = fpu_round(f, v);
    if (!(r >= -32768.0 && r <= 32767.0)) return std::numeric_limits<std::int16_t>::min();
    return std::int16_t(r);
}
inline std::int64_t fpu_to_i64(const Fpu& f, double v) {
    double r = fpu_round(f, v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(r);
}

// ---- shifts / rotates with x86 count masking and flag rules ----
struct ShiftFlags { std::uint8_t cf, of; };

}

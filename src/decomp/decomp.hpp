#pragma once
// Hand-decompiled game functions (Phase 6).
//
// Each one replaces a translated function: VOY_DECOMP(program, name) defines
// the replacement, a cdecl callee like the original (process.hpp: Args,
// ret, ret_fp). tools/x86tr/translate.py finds the markers in src/decomp
// and makes the translated f_<address> call the decompiled version, keeping
// the translation as orig_<address>: VOY_DECOMP=0 runs the translations
// again (to bisect a difference), and tests/decomp_diff.cpp compares the two
// on the same inputs.
//
// The game's data still lives in guest memory, so the decompiled code reads
// and writes it through guest pointers (V3 / M4 below). x87 arithmetic was
// 64-bit precision; the decompiled code computes in double and rounds to
// float where the original stored a float.
#include <cstdint>

#include "host/process.hpp"
#include "recomp/cpu.hpp"

namespace voy::decomp {

// Whether decompiled functions are used (VOY_DECOMP=0 turns them off).
bool enabled();

struct Vec3 {
    float x, y, z;
};
static_assert(sizeof(Vec3) == 12);

inline Vec3& V3(recomp::Cpu& c, std::uint32_t guest) { return *reinterpret_cast<Vec3*>(c.mem + guest); }
inline float* F(recomp::Cpu& c, std::uint32_t guest) { return reinterpret_cast<float*>(c.mem + guest); }

}

#define VOY_DECOMP(program, name) void program##_##name(::voy::recomp::Cpu& c)

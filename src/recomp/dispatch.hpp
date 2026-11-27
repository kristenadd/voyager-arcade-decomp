#pragma once
// Calls whose target is only known at run time: function pointers, the PLT's
// jumps through the GOT, GLUT callbacks. Each guest program (trek, stvsnd,
// stvserv) has its own sorted table of translated entry points; the Cpu
// names the Process it belongs to (Cpu::process), so two programs can run at
// once on two host threads. Anything outside the table fails closed.
#include <cstdint>
#include <initializer_list>
#include <span>

#include "recomp/cpu.hpp"

namespace voy::recomp {

using GuestFn = void (*)(Cpu&);

struct GuestEntry {
    std::uint32_t address;
    GuestFn fn;
};

// GOT slots of imported functions hold addresses in this range at load:
// import k is ImportBase + 16 * k.
constexpr std::uint32_t ImportBase = 0xFFF00000u;

// A decompiled function (src/decomp) and the translation it replaced, for
// the differential test.
struct DecompPair {
    std::uint32_t address;
    const char* name;
    GuestFn translated;
};

// One imported function. `handler` is the native implementation (a cdecl
// callee: arguments at [esp+4]..., result in eax or st(0), esp moves past the
// return address only). A null handler fails closed and names `name`.
struct ImportSlot {
    std::uint32_t got = 0;
    const char* name = nullptr;
    GuestFn handler = nullptr;
};

// What dispatch needs from a process. Kept in the Cpu (Cpu::process).
struct DispatchTables {
    std::span<const GuestEntry> entries;
    std::span<const ImportSlot> imports;
};

// Calls the guest function at `target` (the return address is already pushed).
void call_indirect(Cpu& c, std::uint32_t target);
// Looks up a translated function; nullptr when there is none.
GuestFn find_entry(const Cpu& c, std::uint32_t target);
// Calls a guest cdecl function from host code: `args` are pushed right to
// left, then a return address. Returns eax; esp is restored.
std::uint32_t call_guest(Cpu& c, std::uint32_t target, std::initializer_list<std::uint32_t> args);

}

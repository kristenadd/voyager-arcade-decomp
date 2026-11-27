// The runtime entry points generated code calls besides dispatch.
#include "recomp/guest.hpp"

#include <chrono>
#include <cstdio>

#include "host/fail.hpp"
#include "host/machine.hpp"

namespace voy::recomp {

std::atomic<bool> g_preempt{false};
std::uint64_t g_probe[8];

void trap(Cpu& c, std::uint32_t va, const char* what) {
    (void)c;
    host::fail_closed(host::Unported::Instruction, va, what);
}

void guest_unported(Cpu& c, std::uint32_t va, const char* why) {
    (void)c;
    host::fail_closed(host::Unported::Function, va, why);
}

// A Pentium III with MMX and SSE, like the cabinet's; the game never asks.
void guest_cpuid(Cpu& c) {
    switch (c.r[Cpu::EAX]) {
    case 0: c.r[Cpu::EAX] = 2; c.r[Cpu::EBX] = 0x756E6547; c.r[Cpu::EDX] = 0x49656E69; c.r[Cpu::ECX] = 0x6C65746E; break;
    case 1: c.r[Cpu::EAX] = 0x683; c.r[Cpu::EBX] = 0; c.r[Cpu::ECX] = 0; c.r[Cpu::EDX] = 0x0383F9FF; break;
    default: c.r[Cpu::EAX] = c.r[Cpu::EBX] = c.r[Cpu::ECX] = c.r[Cpu::EDX] = 0; break;
    }
}

void guest_pause(Cpu&) {}
void guest_preempt(Cpu&) {}
void debug_probe(Cpu&, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) {}
void nan_trap_report(Cpu&, std::uint32_t site) { std::fprintf(stderr, "[nan] first NaN stored at %08X\n", site); }

// A 1 GHz time stamp counter (native_get_cpu_mhz reports 1000 MHz), on the
// machine's clock: real time, or virtual time in headless runs.
std::uint64_t guest_rdtsc() { return host::clock_ns(); }

}

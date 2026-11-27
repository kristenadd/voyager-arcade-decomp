#pragma once
// Fail closed: anything the port does not implement stops the program with a
// report that names it, instead of guessing. A guest program that stops this
// way unwinds to its thread's root (GuestStop), so the host can report it.
#include <cstdint>
#include <string>

namespace voy::host {

enum class Unported { Import, Function, Instruction, Device, Call, Format };

// Thrown to unwind a guest program: exit(), a fail-closed stop, or a host request.
struct GuestStop {
    int code;
    std::string why;
};

[[noreturn]] void fail_closed(Unported kind, std::uint32_t where, const std::string& what);
// Logs once per (kind, key) and continues: for state the port accepts but ignores.
void note_once(const char* kind, const std::string& key);
// The port's log (stderr and, when set, a file in the container).
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}

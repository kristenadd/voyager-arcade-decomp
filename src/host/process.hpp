#pragma once
// One guest program (trek, stvsnd or stvserv): its 4 GB guest address space
// with the ELF image loaded at its link address, a heap, a stack, the GOT
// bound to native import handlers, and the program's slice of the operating
// system (open files and descriptors, environment, atexit handlers).
//
// Guest layout:
//   0x08048000  the ELF image (.text, .rodata, .data, .bss)
//   0x0F000000  host-made objects: argv, FILE handles, string results (bump)
//   0x10000000  the heap (malloc), committed as it grows
//   0xBF800000  the stack (8 MB, top at 0xC0000000)
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp/cpu.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/guest_memory.hpp"
#include "recomp/x86ops.hpp"

namespace voy::host {

class Machine;
struct OsState;

constexpr std::uint32_t kHostObjects = 0x0F000000u, kHostObjectsEnd = 0x10000000u;
constexpr std::uint32_t kHeapBase = 0x10000000u, kHeapEnd = 0xB0000000u;
constexpr std::uint32_t kStackTop = 0xC0000000u, kStackSize = 8u << 20;

class Process {
public:
    Process(std::string name, Machine& machine);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Loads the ELF at `path`, verified by the caller, with its translated code.
    bool load(const std::string& path, std::span<const recomp::GuestEntry> entries, std::string* error);
    // Runs the program from its ELF entry (_start → __libc_start_main → main)
    // with these arguments. Returns the exit status; a fail-closed stop is -2.
    int run(const std::vector<std::string>& argv);
    // Asks the program to stop at its next import call (a GuestStop with `code`).
    void request_stop(int code) { stop_code_ = code; stop_requested_ = true; }
    bool stop_requested() const { return stop_requested_; }
    int stop_code() const { return stop_code_; }

    const std::string& name() const { return name_; }
    Machine& machine() { return machine_; }
    recomp::Cpu& cpu() { return cpu_; }
    std::uint8_t* mem() const { return mem_.base(); }
    const recomp::DispatchTables& dispatch() const { return tables_; }
    OsState& os() { return *os_; }

    // Address of a symbol in the program's symbol table (0 when absent).
    std::uint32_t sym(const std::string& name) const;
    // The function containing `va`, for reports ("name+0x12").
    std::string describe(std::uint32_t va) const;

    // Guest heap.
    std::uint32_t malloc(std::uint32_t n);
    void free(std::uint32_t p);
    std::uint32_t realloc(std::uint32_t p, std::uint32_t n);
    std::uint32_t calloc(std::uint32_t n, std::uint32_t size);
    // Permanent host-made guest objects (never freed).
    std::uint32_t host_alloc(std::uint32_t n, std::uint32_t align = 16);
    std::uint32_t host_string(const std::string& s);

    // Guest memory helpers.
    std::string str(std::uint32_t a, std::size_t limit = 1u << 20) const;
    std::uint32_t u32(std::uint32_t a) const { return recomp::ld32(mem_.base(), a); }
    void set_u32(std::uint32_t a, std::uint32_t v) { recomp::st32(mem_.base(), a, v); }
    void* host(std::uint32_t a) const { return mem_.host(a); }

    // exit(): runs the atexit handlers, then unwinds the program.
    [[noreturn]] void exit(int code);
    void add_atexit(std::uint32_t fn, std::uint32_t arg) { atexit_.push_back({fn, arg}); }

private:
    bool commit_heap(std::uint32_t end);

    std::string name_;
    Machine& machine_;
    recomp::GuestMemory mem_;
    recomp::Cpu cpu_;
    recomp::DispatchTables tables_;
    std::vector<recomp::ImportSlot> imports_;
    std::unordered_map<std::string, std::uint32_t> symbols_;
    std::map<std::uint32_t, std::string> functions_;
    std::uint32_t entry_ = 0;
    std::unique_ptr<OsState> os_;
    std::uint32_t host_next_ = kHostObjects;
    std::uint32_t heap_top_ = kHeapBase, heap_committed_ = kHeapBase;
    std::vector<std::vector<std::uint32_t>> free_lists_;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> atexit_;
    bool in_exit_ = false;
    volatile bool stop_requested_ = false;
    int stop_code_ = 0;
};

// Native import handlers, by name (libc.cpp, gl.cpp, glut.cpp register theirs).
using ImportHandler = recomp::GuestFn;
void register_imports(std::initializer_list<std::pair<const char*, ImportHandler>> handlers);
ImportHandler find_import(const std::string& name);

// The Process a guest CPU belongs to.
inline Process& proc(recomp::Cpu& c) { return *static_cast<Process*>(c.process); }

// Argument access for cdecl import handlers: [esp] is the return address.
struct Args {
    recomp::Cpu& c;
    std::uint32_t u(int i) const { return recomp::ld32(c.mem, c.r[recomp::Cpu::ESP] + 4 + 4 * i); }
    std::int32_t i(int k) const { return std::int32_t(u(k)); }
    float f(int k) const { return recomp::ldf32(c.mem, c.r[recomp::Cpu::ESP] + 4 + 4 * k); }
    // A double starting at dword slot k (it takes slots k and k + 1).
    double d(int k) const { return recomp::ldf64(c.mem, c.r[recomp::Cpu::ESP] + 4 + 4 * k); }
    std::uint32_t at(int k) const { return c.r[recomp::Cpu::ESP] + 4 + 4 * k; }
};
inline void ret(recomp::Cpu& c, std::uint32_t v) {
    c.r[recomp::Cpu::EAX] = v;
    c.r[recomp::Cpu::ESP] += 4;
}
inline void ret_void(recomp::Cpu& c) { c.r[recomp::Cpu::ESP] += 4; }
inline void ret_fp(recomp::Cpu& c, double v) {
    c.fpu.push(v);
    c.r[recomp::Cpu::ESP] += 4;
}

}

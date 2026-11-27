// The glibc 2.2 functions the three programs import, over guest memory.
// Each handler is a cdecl callee (process.hpp: Args, ret).
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include "host/fail.hpp"
#include "host/format.hpp"
#include "host/machine.hpp"
#include "host/os.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

namespace {

char* gp(Cpu& c, std::uint32_t a) { return reinterpret_cast<char*>(c.mem + a); }

// ---- process ----
void h_libc_start_main(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::uint32_t main = a.u(0), argc = a.u(1), argv = a.u(2), init = a.u(3);
    if (init) call_guest(c, init, {});
    const std::uint32_t envp = argv + 4 * (argc + 1);
    const std::uint32_t r = call_guest(c, main, {argc, argv, envp});
    p.exit(int(r));
}
void h_exit(Cpu& c) { proc(c).exit(Args{c}.i(0)); }
void h_assert_fail(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    logf("[%s] assertion failed: %s (%s:%d, %s)\n", p.name().c_str(), p.str(a.u(0)).c_str(), p.str(a.u(1)).c_str(),
         a.i(2), p.str(a.u(3)).c_str());
    p.exit(134);
}
void h_cxa_atexit(Cpu& c) {
    Args a{c};
    proc(c).add_atexit(a.u(0), a.u(1));
    ret(c, 0);
}
void h_nop0(Cpu& c) { ret(c, 0); }
void h_getenv(Cpu& c) {
    Process& p = proc(c);
    auto& os = p.os();
    const std::string name = p.str(Args{c}.u(0));
    auto it = os.env.find(name);
    if (it == os.env.end()) return ret(c, 0);
    auto& s = os.env_strings[name + "=" + it->second];
    if (!s) s = p.host_string(it->second);
    ret(c, s);
}
void h_putenv(Cpu& c) {
    Process& p = proc(c);
    const std::string kv = p.str(Args{c}.u(0));
    const auto eq = kv.find('=');
    if (eq != std::string::npos) p.os().env[kv.substr(0, eq)] = kv.substr(eq + 1);
    ret(c, 0);
}
void h_geteuid(Cpu& c) { ret(c, 1000); }  // not root: verify_ib is native anyway
void h_system(Cpu& c) {
    Process& p = proc(c);
    note_once("system", p.name() + ": " + p.str(Args{c}.u(0)));
    ret(c, 0);
}
void h_sleep(Cpu& c) {
    const std::uint32_t s = Args{c}.u(0);
    if (s) std::this_thread::sleep_for(std::chrono::seconds(std::min<std::uint32_t>(s, 5)));
    ret(c, 0);
}
void h_usleep(Cpu& c) {
    const std::uint32_t us = Args{c}.u(0);
    if (us) std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::uint32_t>(us, 1000000)));
    ret(c, 0);
}
void h_time(Cpu& c) {
    const std::uint32_t t = std::uint32_t(std::time(nullptr));
    const std::uint32_t out = Args{c}.u(0);
    if (out) st32(c.mem, out, t);
    ret(c, t);
}

// ---- memory ----
void h_malloc(Cpu& c) { ret(c, proc(c).malloc(Args{c}.u(0))); }
void h_calloc(Cpu& c) { Args a{c}; ret(c, proc(c).calloc(a.u(0), a.u(1))); }
void h_realloc(Cpu& c) { Args a{c}; ret(c, proc(c).realloc(a.u(0), a.u(1))); }
void h_free(Cpu& c) { proc(c).free(Args{c}.u(0)); ret_void(c); }
void h_memcpy(Cpu& c) {
    Args a{c};
    std::memmove(gp(c, a.u(0)), gp(c, a.u(1)), a.u(2));
    ret(c, a.u(0));
}
void h_memset(Cpu& c) {
    Args a{c};
    std::memset(gp(c, a.u(0)), int(a.u(1) & 0xFF), a.u(2));
    ret(c, a.u(0));
}

// ---- strings ----
void h_strcpy(Cpu& c) {
    Args a{c};
    std::memmove(gp(c, a.u(0)), gp(c, a.u(1)), std::strlen(gp(c, a.u(1))) + 1);
    ret(c, a.u(0));
}
void h_strncpy(Cpu& c) {
    Args a{c};
    std::strncpy(gp(c, a.u(0)), gp(c, a.u(1)), a.u(2));
    ret(c, a.u(0));
}
void h_strcat(Cpu& c) {
    Args a{c};
    char* d = gp(c, a.u(0));
    std::memmove(d + std::strlen(d), gp(c, a.u(1)), std::strlen(gp(c, a.u(1))) + 1);
    ret(c, a.u(0));
}
void h_strcmp(Cpu& c) { Args a{c}; ret(c, std::uint32_t(std::strcmp(gp(c, a.u(0)), gp(c, a.u(1))))); }
void h_strncmp(Cpu& c) { Args a{c}; ret(c, std::uint32_t(std::strncmp(gp(c, a.u(0)), gp(c, a.u(1)), a.u(2)))); }
void h_strchr(Cpu& c) {
    Args a{c};
    const char* s = gp(c, a.u(0));
    const char* r = std::strchr(s, int(a.u(1) & 0xFF));
    ret(c, r ? a.u(0) + std::uint32_t(r - s) : 0);
}
void h_strstr(Cpu& c) {
    Args a{c};
    const char* s = gp(c, a.u(0));
    const char* r = std::strstr(s, gp(c, a.u(1)));
    ret(c, r ? a.u(0) + std::uint32_t(r - s) : 0);
}

// ---- formatted output ----
void h_sprintf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string s = guest_vformat(p, a.u(1), a.at(2));
    std::memcpy(p.host(a.u(0)), s.c_str(), s.size() + 1);
    ret(c, std::uint32_t(s.size()));
}
void h_vsprintf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string s = guest_vformat(p, a.u(1), a.u(2));
    std::memcpy(p.host(a.u(0)), s.c_str(), s.size() + 1);
    ret(c, std::uint32_t(s.size()));
}
void h_snprintf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string s = guest_vformat(p, a.u(2), a.at(3));
    const std::uint32_t n = a.u(1);
    if (n) {
        const std::size_t k = std::min<std::size_t>(s.size(), n - 1);
        std::memcpy(p.host(a.u(0)), s.data(), k);
        static_cast<char*>(p.host(a.u(0)))[k] = 0;
    }
    ret(c, std::uint32_t(s.size()));
}
void put_stdout(Process& p, const std::string& s) {
    if (p.machine().config().stdout_echo) logf("[%s] %s", p.name().c_str(), s.c_str());
}
void h_printf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string s = guest_vformat(p, a.u(0), a.at(1));
    put_stdout(p, s);
    ret(c, std::uint32_t(s.size()));
}
void h_perror(Cpu& c) {
    Process& p = proc(c);
    put_stdout(p, p.str(Args{c}.u(0)) + ": error\n");
    ret_void(c);
}

// ---- formatted input ----
void h_sscanf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    StringScanSource src(p.str(a.u(0)));
    ret(c, std::uint32_t(guest_vscan(p, src, a.u(1), a.at(2))));
}

// ---- math (double in, double out in st(0)) ----
template <double (*F)(double)> void h_math(Cpu& c) { ret_fp(c, F(Args{c}.d(0))); }
double m_sin(double x) { return std::sin(x); }
double m_cos(double x) { return std::cos(x); }
double m_tan(double x) { return std::tan(x); }
double m_asin(double x) { return std::asin(x); }
double m_acos(double x) { return std::acos(x); }
double m_exp(double x) { return std::exp(x); }
double m_log(double x) { return std::log(x); }

void h_rand(Cpu& c) { ret(c, std::uint32_t(proc(c).os().rand())); }
void h_srand(Cpu& c) { proc(c).os().srand(Args{c}.u(0)); ret_void(c); }

// qsort with a guest comparator: the elements are copied to a guest scratch
// buffer, an index is sorted with the comparator called on scratch elements,
// and the elements are written back in order.
void h_qsort(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::uint32_t base = a.u(0), n = a.u(1), size = a.u(2), cmp = a.u(3);
    if (n > 1 && size) {
        const std::uint32_t scratch = p.malloc(n * size);
        std::memcpy(p.host(scratch), p.host(base), std::size_t(n) * size);
        std::vector<std::uint32_t> idx(n);
        for (std::uint32_t i = 0; i < n; ++i) idx[i] = i;
        const std::uint32_t saved_esp = c.r[Cpu::ESP];
        std::stable_sort(idx.begin(), idx.end(), [&](std::uint32_t x, std::uint32_t y) {
            return std::int32_t(call_guest(c, cmp, {scratch + x * size, scratch + y * size})) < 0;
        });
        c.r[Cpu::ESP] = saved_esp;
        for (std::uint32_t i = 0; i < n; ++i)
            std::memcpy(p.host(base + i * size), p.host(scratch + idx[i] * size), size);
        p.free(scratch);
    }
    ret_void(c);
}

// ---- termios, privileged I/O: accepted ----
void h_tcgetattr(Cpu& c) {
    Args a{c};
    std::memset(gp(c, a.u(1)), 0, 60);
    ret(c, 0);
}
void h_iopl(Cpu& c) { ret(c, 0); }

}  // namespace

void register_libc_imports() {
    register_imports({
        {"__libc_start_main", h_libc_start_main},
        {"exit", h_exit},
        {"__assert_fail", h_assert_fail},
        {"__cxa_atexit", h_cxa_atexit},
        {"__register_frame_info", h_nop0},
        {"__deregister_frame_info", h_nop0},
        {"getenv", h_getenv},
        {"putenv", h_putenv},
        {"geteuid", h_geteuid},
        {"system", h_system},
        {"sleep", h_sleep},
        {"usleep", h_usleep},
        {"time", h_time},
        {"malloc", h_malloc},
        {"calloc", h_calloc},
        {"realloc", h_realloc},
        {"free", h_free},
        {"memcpy", h_memcpy},
        {"memmove", h_memcpy},
        {"memset", h_memset},
        {"strcpy", h_strcpy},
        {"strncpy", h_strncpy},
        {"strcat", h_strcat},
        {"strcmp", h_strcmp},
        {"strncmp", h_strncmp},
        {"strchr", h_strchr},
        {"strstr", h_strstr},
        {"sprintf", h_sprintf},
        {"vsprintf", h_vsprintf},
        {"snprintf", h_snprintf},
        {"printf", h_printf},
        {"perror", h_perror},
        {"sscanf", h_sscanf},
        {"sin", h_math<m_sin>},
        {"cos", h_math<m_cos>},
        {"tan", h_math<m_tan>},
        {"asin", h_math<m_asin>},
        {"acos", h_math<m_acos>},
        {"exp", h_math<m_exp>},
        {"log", h_math<m_log>},
        {"rand", h_rand},
        {"srand", h_srand},
        {"qsort", h_qsort},
        {"tcgetattr", h_tcgetattr},
        {"tcsetattr", h_nop0},
        {"setvbuf", h_nop0},
        {"iopl", h_iopl},
    });
}

}

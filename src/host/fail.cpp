#include "host/fail.hpp"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <set>

namespace voy::host {

namespace {
std::mutex g_mutex;
std::set<std::string> g_noted;
const char* kind_name(Unported k) {
    switch (k) {
    case Unported::Import: return "import";
    case Unported::Function: return "function";
    case Unported::Instruction: return "instruction";
    case Unported::Device: return "device";
    case Unported::Call: return "call";
    case Unported::Format: return "format";
    }
    return "?";
}
}  // namespace

void logf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void fail_closed(Unported kind, std::uint32_t where, const std::string& what) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X", where);
    std::string why = std::string("unported ") + kind_name(kind) + " at " + buf + ": " + what;
    logf("[fail-closed] %s\n", why.c_str());
    throw GuestStop{-2, why};
}

void note_once(const char* kind, const std::string& key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_noted.insert(std::string(kind) + ":" + key).second) std::fprintf(stderr, "[note] %s: %s\n", kind, key.c_str());
}

}

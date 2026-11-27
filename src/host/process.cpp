#include "host/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>

#include "host/fail.hpp"
#include "host/os.hpp"

namespace voy::host {

using namespace recomp;

namespace {

std::mutex g_imports_mutex;
std::unordered_map<std::string, ImportHandler>& import_registry() {
    static std::unordered_map<std::string, ImportHandler> r;
    return r;
}

template <class T> T rd(const std::vector<std::uint8_t>& b, std::size_t o) {
    T v;
    std::memcpy(&v, b.data() + o, sizeof v);
    return v;
}

constexpr std::uint32_t R_386_GLOB_DAT = 6, R_386_JUMP_SLOT = 7, R_386_COPY = 5;

// Size classes: multiples of 16 up to 4 KB, then powers of two.
std::uint32_t size_class(std::uint32_t n, std::uint32_t* rounded) {
    if (n <= 4096) {
        std::uint32_t r = (n + 15) & ~15u;
        if (r == 0) r = 16;
        *rounded = r;
        return r / 16;
    }
    std::uint32_t r = 8192, k = 0;
    while (r < n) r <<= 1, ++k;
    *rounded = r;
    return 257 + k;
}

}  // namespace

void register_imports(std::initializer_list<std::pair<const char*, ImportHandler>> handlers) {
    std::lock_guard<std::mutex> lock(g_imports_mutex);
    for (auto& [name, fn] : handlers) import_registry()[name] = fn;
}

ImportHandler find_import(const std::string& name) {
    std::lock_guard<std::mutex> lock(g_imports_mutex);
    auto it = import_registry().find(name);
    return it == import_registry().end() ? nullptr : it->second;
}

Process::Process(std::string name, Machine& machine) : name_(std::move(name)), machine_(machine), os_(new OsState) {
    free_lists_.resize(300);
}

Process::~Process() = default;

bool Process::load(const std::string& path, std::span<const GuestEntry> entries, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 52 || std::memcmp(b.data(), "\x7f" "ELF", 4) != 0 || b[4] != 1) {
        *error = path + ": not an ELF32 file";
        return false;
    }
    if (!mem_.reserve(error)) return false;
    entry_ = rd<std::uint32_t>(b, 24);
    const std::uint32_t phoff = rd<std::uint32_t>(b, 28), shoff = rd<std::uint32_t>(b, 32);
    const std::uint16_t phentsize = rd<std::uint16_t>(b, 42), phnum = rd<std::uint16_t>(b, 44);
    const std::uint16_t shentsize = rd<std::uint16_t>(b, 46), shnum = rd<std::uint16_t>(b, 48);
    for (unsigned i = 0; i < phnum; ++i) {
        const std::size_t p = phoff + i * phentsize;
        if (rd<std::uint32_t>(b, p) != 1) continue;  // PT_LOAD
        const std::uint32_t off = rd<std::uint32_t>(b, p + 4), va = rd<std::uint32_t>(b, p + 8);
        const std::uint32_t filesz = rd<std::uint32_t>(b, p + 16), memsz = rd<std::uint32_t>(b, p + 20);
        if (!mem_.commit(va, memsz, error)) return false;
        std::memcpy(mem_.host(va), b.data() + off, filesz);
    }
    // Sections: the symbol tables and the relocations.
    struct Sh { std::uint32_t name, type, flags, addr, off, size, link, info, align, entsize; };
    std::vector<Sh> sh(shnum);
    for (unsigned i = 0; i < shnum; ++i) std::memcpy(&sh[i], b.data() + shoff + i * shentsize, sizeof(Sh));
    auto cstr = [&](std::size_t o) { return std::string(reinterpret_cast<const char*>(b.data() + o)); };
    const Sh* dynsym = nullptr;
    for (auto& s : sh) {
        if (s.type == 2) {  // SHT_SYMTAB
            const Sh& strtab = sh[s.link];
            for (std::uint32_t k = 0; k < s.size / 16; ++k) {
                const std::size_t e = s.off + k * 16;
                const std::string n = cstr(strtab.off + rd<std::uint32_t>(b, e));
                const std::uint32_t value = rd<std::uint32_t>(b, e + 4);
                const std::uint8_t info = b[e + 12];
                if (n.empty() || !value) continue;
                std::string key = n.substr(0, n.find('@'));
                symbols_.emplace(key, value);
                if ((info & 0xF) == 2) functions_[value] = key;
            }
        }
        if (s.type == 11) dynsym = &s;  // SHT_DYNSYM
    }
    std::uint32_t host_objects_end = kHostObjectsEnd;
    if (!mem_.commit(kHostObjects, host_objects_end - kHostObjects, error)) return false;
    if (!mem_.commit(kStackTop - kStackSize, kStackSize, error)) return false;
    for (auto& s : sh) {
        if (s.type != 9 || !dynsym) continue;  // SHT_REL
        const Sh& dstr = sh[dynsym->link];
        for (std::uint32_t k = 0; k < s.size / 8; ++k) {
            const std::uint32_t slot = rd<std::uint32_t>(b, s.off + k * 8), info = rd<std::uint32_t>(b, s.off + k * 8 + 4);
            const std::size_t se = dynsym->off + (info >> 8) * 16;
            const std::string n = cstr(dstr.off + rd<std::uint32_t>(b, se));
            const std::uint32_t type = info & 0xFF;
            if (type == R_386_JUMP_SLOT || type == R_386_GLOB_DAT) {
                if (n == "__gmon_start__" || n == "__dso_handle") {
                    set_u32(slot, 0);
                    continue;
                }
                ImportSlot is;
                is.got = slot;
                is.name = strdup(n.c_str());
                is.handler = find_import(n);
                set_u32(slot, ImportBase + 16 * std::uint32_t(imports_.size()));
                imports_.push_back(is);
            } else if (type == R_386_COPY) {
                // libc data the program reads directly: stdout (a FILE*) and
                // __ctype_toupper (a pointer to an int table indexed -128..255).
                if (n == "stdout") {
                    set_u32(slot, os_->stdout_file(*this));
                } else if (n == "__ctype_toupper") {
                    const std::uint32_t table = host_alloc(384 * 4);
                    for (int c = -128; c < 256; ++c) {
                        int v = c;
                        if (c >= 'a' && c <= 'z') v = c - 32;
                        st32(mem(), table + std::uint32_t(c + 128) * 4, std::uint32_t(v));
                    }
                    set_u32(slot, table + 128 * 4);
                } else {
                    *error = "unknown copied libc object " + n;
                    return false;
                }
            }
        }
    }
    tables_.entries = entries;
    tables_.imports = imports_;
    cpu_.mem = mem_.base();
    cpu_.process = this;
    cpu_.r[Cpu::ESP] = kStackTop - 64;
    return true;
}

std::uint32_t Process::sym(const std::string& name) const {
    auto it = symbols_.find(name);
    return it == symbols_.end() ? 0 : it->second;
}

std::string Process::describe(std::uint32_t va) const {
    auto it = functions_.upper_bound(va);
    if (it == functions_.begin()) return "?";
    --it;
    char buf[32];
    std::snprintf(buf, sizeof buf, "+0x%X", va - it->first);
    return it->second + buf;
}

int Process::run(const std::vector<std::string>& argv) {
    std::vector<std::uint32_t> ptrs;
    for (auto& a : argv) ptrs.push_back(host_string(a));
    // The initial stack _start expects: argc, argv[], 0, envp[] (empty), 0.
    std::uint32_t sp = kStackTop - 4096;
    sp &= ~15u;
    std::vector<std::uint32_t> words;
    words.push_back(std::uint32_t(ptrs.size()));
    words.insert(words.end(), ptrs.begin(), ptrs.end());
    words.push_back(0);
    words.push_back(0);
    sp -= std::uint32_t(words.size() * 4);
    for (std::size_t k = 0; k < words.size(); ++k) set_u32(sp + std::uint32_t(k * 4), words[k]);
    cpu_.r[Cpu::ESP] = sp;
    try {
        call_indirect(cpu_, entry_);
        return 0;  // _start never returns; __libc_start_main exits
    } catch (const GuestStop& s) {
        if (s.code == -2) logf("[%s] stopped: %s\n", name_.c_str(), s.why.c_str());
        return s.code;
    }
}

void Process::exit(int code) {
    if (!in_exit_) {
        in_exit_ = true;
        try {
            while (!atexit_.empty()) {
                auto [fn, arg] = atexit_.back();
                atexit_.pop_back();
                call_guest(cpu_, fn, {arg});
            }
        } catch (const GuestStop&) {
        }
        os_->close_all(*this);
    }
    throw GuestStop{code, "exit"};
}

bool Process::commit_heap(std::uint32_t end) {
    if (end <= heap_committed_) return true;
    if (end > kHeapEnd) return false;
    const std::uint32_t chunk = 16u << 20;
    std::uint32_t to = (end + chunk - 1) & ~(chunk - 1);
    if (to > kHeapEnd) to = kHeapEnd;
    std::string err;
    if (!mem_.commit(heap_committed_, to - heap_committed_, &err)) return false;
    heap_committed_ = to;
    return true;
}

// Each block has a 16-byte header: size class rounded size, then a magic word.
std::uint32_t Process::malloc(std::uint32_t n) {
    std::uint32_t rounded;
    const std::uint32_t cls = size_class(n, &rounded);
    auto& list = free_lists_[cls];
    if (!list.empty()) {
        const std::uint32_t p = list.back();
        list.pop_back();
        st32(mem(), p - 12, 0x4D4C4C43u);  // in use
        return p;
    }
    const std::uint32_t p = heap_top_ + 16;
    if (!commit_heap(p + rounded)) {
        logf("[%s] guest heap exhausted (%u bytes asked)\n", name_.c_str(), n);
        return 0;
    }
    heap_top_ = p + rounded;
    set_u32(p - 16, rounded);
    set_u32(p - 12, 0x4D4C4C43u);
    return p;
}

void Process::free(std::uint32_t p) {
    if (!p) return;
    if (p < kHeapBase || p >= heap_top_ || u32(p - 12) != 0x4D4C4C43u) {
        note_once("free", name_ + ": free of a pointer the heap did not give out or freed twice");
        return;
    }
    set_u32(p - 12, 0x46524545u);  // free
    std::uint32_t rounded;
    const std::uint32_t cls = size_class(u32(p - 16), &rounded);
    free_lists_[cls].push_back(p);
}

std::uint32_t Process::realloc(std::uint32_t p, std::uint32_t n) {
    if (!p) return malloc(n);
    if (!n) {
        free(p);
        return 0;
    }
    const std::uint32_t have = u32(p - 16);
    if (n <= have) return p;
    const std::uint32_t q = malloc(n);
    if (!q) return 0;
    std::memcpy(host(q), host(p), have);
    free(p);
    return q;
}

std::uint32_t Process::calloc(std::uint32_t n, std::uint32_t size) {
    const std::uint64_t total = std::uint64_t(n) * size;
    if (total > 0x40000000u) return 0;
    const std::uint32_t p = malloc(std::uint32_t(total));
    if (p) std::memset(host(p), 0, std::size_t(total));
    return p;
}

std::uint32_t Process::host_alloc(std::uint32_t n, std::uint32_t align) {
    host_next_ = (host_next_ + align - 1) & ~(align - 1);
    const std::uint32_t p = host_next_;
    host_next_ += n;
    if (host_next_ > kHostObjectsEnd) fail_closed(Unported::Call, p, "host object area exhausted");
    std::memset(host(p), 0, n);
    return p;
}

std::uint32_t Process::host_string(const std::string& s) {
    const std::uint32_t p = host_alloc(std::uint32_t(s.size() + 1), 4);
    std::memcpy(host(p), s.c_str(), s.size() + 1);
    return p;
}

std::string Process::str(std::uint32_t a, std::size_t limit) const {
    if (!a) return std::string();
    const char* p = static_cast<const char*>(host(a));
    return std::string(p, strnlen(p, limit));
}

}

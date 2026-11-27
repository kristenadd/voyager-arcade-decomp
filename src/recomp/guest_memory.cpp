#include "recomp/guest_memory.hpp"

#include <sys/mman.h>

#include <cerrno>
#include <cstring>

namespace voy::recomp {

namespace {

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what + ": " + std::strerror(errno);
    return false;
}

std::uint64_t page_floor(std::uint64_t v) { return v & ~std::uint64_t(GuestMemory::PageSize - 1); }
std::uint64_t page_ceil(std::uint64_t v) { return page_floor(v + GuestMemory::PageSize - 1); }

}

GuestMemory::~GuestMemory() {
    if (base_) munmap(base_, Size + Guard);
}

bool GuestMemory::reserve(std::string* error) {
    if (base_) return true;
    void* p = mmap(nullptr, Size + Guard, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return fail(error, "cannot reserve the 4 GB guest address space");
    base_ = static_cast<std::uint8_t*>(p);
    return true;
}

bool GuestMemory::commit(std::uint32_t address, std::uint32_t size, std::string* error) {
    if (!base_) return fail(error, "guest memory not reserved");
    std::uint64_t lo = page_floor(address), hi = page_ceil(std::uint64_t(address) + size);
    if (mprotect(base_ + lo, hi - lo, PROT_READ | PROT_WRITE) != 0) return fail(error, "cannot commit guest memory");
    return true;
}

bool GuestMemory::protect_read_only(std::uint32_t address, std::uint32_t size, std::string* error) {
    // Only whole pages inside the range: a page shared with writable data stays writable.
    std::uint64_t lo = page_ceil(address), hi = page_floor(std::uint64_t(address) + size);
    if (hi <= lo) return true;
    if (mprotect(base_ + lo, hi - lo, PROT_READ) != 0) return fail(error, "cannot protect guest memory");
    return true;
}

}

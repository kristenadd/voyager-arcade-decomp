#pragma once
// The guest's 32-bit address space: one 4 GB reservation (plus a guard so an
// access at 0xFFFFFFFF of up to 16 bytes stays inside), with pages
// committed as the guest needs them. Guest pointers stay 32-bit values;
// host code converts with host() at the point of use and never stores a
// host pointer in guest memory.
#include <cstddef>
#include <cstdint>
#include <string>

namespace voy::recomp {

class GuestMemory {
public:
    static constexpr std::uint64_t Size = 1ull << 32;
    static constexpr std::uint64_t Guard = 1ull << 16;
    static constexpr std::uint32_t PageSize = 1u << 14;  // Apple silicon page size

    GuestMemory() = default;
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    // Reserves the window (no pages accessible). False with *error on failure.
    bool reserve(std::string* error);
    // Makes [address, address + size) readable and writable, zero-filled where new.
    bool commit(std::uint32_t address, std::uint32_t size, std::string* error);
    // Makes a committed range read-only (guest code may not write the EXE's constants).
    bool protect_read_only(std::uint32_t address, std::uint32_t size, std::string* error);

    std::uint8_t* base() const { return base_; }
    std::uint8_t* host(std::uint32_t address) const { return base_ + address; }
    // Guest address of a host pointer into the window.
    std::uint32_t guest(const void* p) const { return std::uint32_t(static_cast<const std::uint8_t*>(p) - base_); }
    bool contains_host(const void* p) const {
        auto q = static_cast<const std::uint8_t*>(p);
        return base_ && q >= base_ && q < base_ + Size + Guard;
    }

private:
    std::uint8_t* base_ = nullptr;
};

}

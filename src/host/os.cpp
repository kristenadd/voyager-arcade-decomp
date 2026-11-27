#include "host/os.hpp"

#include <unistd.h>

#include "host/process.hpp"

namespace voy::host {

std::uint32_t OsState::new_file(Process& p, GuestFile f) {
    // A guest FILE* is a small zeroed guest object; programs only pass it back.
    const std::uint32_t ptr = p.host_alloc(0x98, 4);
    files[ptr] = std::move(f);
    return ptr;
}

std::uint32_t OsState::stdout_file(Process& p) {
    if (!stdout_ptr) {
        GuestFile f;
        f.kind = GuestFile::Stdout;
        stdout_ptr = new_file(p, f);
    }
    return stdout_ptr;
}

GuestFile* OsState::file(std::uint32_t ptr) {
    auto it = files.find(ptr);
    return it == files.end() ? nullptr : &it->second;
}

int OsState::new_fd(GuestFd fd) {
    if (fds.size() < 3) fds.resize(3);
    fds[0].kind = GuestFd::Stdin;
    fds[1].kind = fds[2].kind = GuestFd::Stdout;
    for (std::size_t i = 3; i < fds.size(); ++i)
        if (fds[i].kind == GuestFd::None) {
            fds[i] = std::move(fd);
            return int(i);
        }
    fds.push_back(std::move(fd));
    return int(fds.size() - 1);
}

GuestFd* OsState::fd(int n) {
    if (fds.size() < 3) {
        fds.resize(3);
        fds[0].kind = GuestFd::Stdin;
        fds[1].kind = fds[2].kind = GuestFd::Stdout;
    }
    if (n < 0 || std::size_t(n) >= fds.size() || fds[n].kind == GuestFd::None) return nullptr;
    return &fds[n];
}

void OsState::close_all(Process&) {
    for (auto& [ptr, f] : files) {
        if (f.fp) std::fclose(f.fp);
        f.fp = nullptr;
        if (f.dev) f.dev->close();
        f.dev.reset();
    }
    for (auto& fd : fds) {
        if (fd.kind == GuestFd::File && fd.host >= 0) ::close(fd.host);
        if (fd.dev) fd.dev->close();
        fd = GuestFd{};
    }
}

// glibc's random_r, TYPE_3 (x**31 + x**3 + 1), as rand() and srand() use it.
void OsState::srand(std::uint32_t seed) {
    std::int32_t* r = rand_r;
    if (seed == 0) seed = 1;
    r[0] = std::int32_t(seed);
    std::int32_t word = std::int32_t(seed);
    for (int i = 1; i < 31; ++i) {
        const std::int32_t hi = word / 127773, lo = word % 127773;
        word = 16807 * lo - 2836 * hi;
        if (word < 0) word += 2147483647;
        r[i] = word;
    }
    rand_f = 3;
    rand_b = 0;
    rand_seeded = true;
    for (int i = 0; i < 310; ++i) rand();
}

std::int32_t OsState::rand() {
    if (!rand_seeded) srand(1);
    std::int32_t* r = rand_r;
    const std::uint32_t v = std::uint32_t(r[rand_f]) + std::uint32_t(r[rand_b]);
    r[rand_f] = std::int32_t(v);
    const std::int32_t result = std::int32_t((v >> 1) & 0x7fffffff);
    if (++rand_f >= 31) rand_f = 0;
    if (++rand_b >= 31) rand_b = 0;
    return result;
}

}

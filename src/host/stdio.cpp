// stdio, file descriptors, stat, select and popen for the guest programs.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "host/fail.hpp"
#include "host/format.hpp"
#include "host/machine.hpp"
#include "host/os.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

namespace {

// Linux i386 open flags.
constexpr std::uint32_t L_O_WRONLY = 1, L_O_RDWR = 2, L_O_CREAT = 0x40, L_O_TRUNC = 0x200, L_O_APPEND = 0x400;

bool mode_writes(const std::string& mode) { return mode.find_first_of("wa+") != std::string::npos; }

std::string host_mode(const std::string& mode) {
    std::string m;
    for (char ch : mode)
        if (ch != 't') m += ch;
    if (m.find('b') == std::string::npos) m += 'b';
    return m;
}

void h_fopen(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string path = p.str(a.u(0)), mode = p.str(a.u(1));
    Machine& m = p.machine();
    std::string host;
    if (mode_writes(mode)) {
        host = m.map_write(path);
        // r+ / a+ need the existing contents: start from the game folder's copy.
        if (mode[0] != 'w' && access(host.c_str(), F_OK) != 0) {
            const std::string src = m.map_read(path);
            if (!src.empty() && src != host) {
                if (FILE* in = std::fopen(src.c_str(), "rb")) {
                    if (FILE* out = std::fopen(host.c_str(), "wb")) {
                        char buf[65536];
                        std::size_t n;
                        while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) std::fwrite(buf, 1, n, out);
                        std::fclose(out);
                    }
                    std::fclose(in);
                }
            }
        }
    } else {
        host = m.map_read(path);
    }
    FILE* fp = host.empty() ? nullptr : std::fopen(host.c_str(), host_mode(mode).c_str());
    if (!fp) return ret(c, 0);
    GuestFile f;
    f.fp = fp;
    f.path = path;
    ret(c, p.os().new_file(p, std::move(f)));
}

GuestFile* need_file(Process& p, std::uint32_t ptr, const char* what) {
    GuestFile* f = p.os().file(ptr);
    if (!f) fail_closed(Unported::Call, ptr, std::string(what) + " on a FILE the port did not open");
    return f;
}

void h_fclose(Cpu& c) {
    Process& p = proc(c);
    const std::uint32_t ptr = Args{c}.u(0);
    GuestFile* f = need_file(p, ptr, "fclose");
    if (f->fp) std::fclose(f->fp);
    if (f->dev) f->dev->close();
    p.os().files.erase(ptr);
    ret(c, 0);
}

void h_fread(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(3), "fread");
    const std::uint32_t size = a.u(1), count = a.u(2);
    if (!size || !count || f->kind != GuestFile::File) return ret(c, 0);
    const std::size_t n = std::fread(p.host(a.u(0)), size, count, f->fp);
    if (n < count) f->eof = std::feof(f->fp) != 0;
    ret(c, std::uint32_t(n));
}

int write_guest_file(Process& p, GuestFile& f, std::uint32_t buf, std::size_t n) {
    switch (f.kind) {
    case GuestFile::File: return int(std::fwrite(p.host(buf), 1, n, f.fp));
    case GuestFile::Stdout:
        if (p.machine().config().stdout_echo) logf("[%s] %.*s", p.name().c_str(), int(n), static_cast<char*>(p.host(buf)));
        return int(n);
    case GuestFile::Device: return f.dev->write(p, buf, std::uint32_t(n));
    }
    return 0;
}

int write_host_string(Process& p, GuestFile& f, const std::string& s) {
    if (f.kind == GuestFile::File) return int(std::fwrite(s.data(), 1, s.size(), f.fp));
    if (f.kind == GuestFile::Stdout) {
        if (p.machine().config().stdout_echo) logf("[%s] %s", p.name().c_str(), s.c_str());
        return int(s.size());
    }
    const std::uint32_t tmp = p.malloc(std::uint32_t(s.size() + 1));
    std::memcpy(p.host(tmp), s.data(), s.size());
    const int r = f.dev->write(p, tmp, std::uint32_t(s.size()));
    p.free(tmp);
    return r;
}

void h_fwrite(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(3), "fwrite");
    const std::uint32_t size = a.u(1), count = a.u(2);
    if (!size || !count) return ret(c, 0);
    const int w = write_guest_file(p, *f, a.u(0), std::size_t(size) * count);
    ret(c, w < 0 ? 0 : std::uint32_t(w) / size);
}

void h_fprintf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(0), "fprintf");
    const std::string s = guest_vformat(p, a.u(1), a.at(2));
    ret(c, std::uint32_t(write_host_string(p, *f, s)));
}

void h_fputc(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(1), "fputc");
    write_host_string(p, *f, std::string(1, char(a.u(0))));
    ret(c, a.u(0) & 0xFF);
}

void h_fgetc(Cpu& c) {
    Process& p = proc(c);
    GuestFile* f = need_file(p, Args{c}.u(0), "fgetc");
    if (f->kind != GuestFile::File) return ret(c, std::uint32_t(EOF));
    const int ch = std::fgetc(f->fp);
    if (ch == EOF) f->eof = true;
    ret(c, std::uint32_t(ch));
}

void h_fgets(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(2), "fgets");
    if (f->kind != GuestFile::File) return ret(c, 0);
    char* r = std::fgets(static_cast<char*>(p.host(a.u(0))), a.i(1), f->fp);
    if (!r) f->eof = std::feof(f->fp) != 0;
    ret(c, r ? a.u(0) : 0);
}

void h_feof(Cpu& c) {
    Process& p = proc(c);
    GuestFile* f = need_file(p, Args{c}.u(0), "feof");
    ret(c, f->kind == GuestFile::File ? (std::feof(f->fp) ? 1u : 0u) : 0u);
}

void h_fflush(Cpu& c) {
    Process& p = proc(c);
    const std::uint32_t ptr = Args{c}.u(0);
    if (GuestFile* f = p.os().file(ptr))
        if (f->fp) std::fflush(f->fp);
    ret(c, 0);
}

void h_fseek(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(0), "fseek");
    if (f->kind != GuestFile::File) return ret(c, std::uint32_t(-1));
    f->eof = false;
    ret(c, std::uint32_t(std::fseek(f->fp, long(a.i(1)), a.i(2))));
}

void h_ftell(Cpu& c) {
    Process& p = proc(c);
    GuestFile* f = need_file(p, Args{c}.u(0), "ftell");
    ret(c, f->kind == GuestFile::File ? std::uint32_t(std::ftell(f->fp)) : 0u);
}

void h_fscanf(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFile* f = need_file(p, a.u(0), "fscanf");
    if (f->kind != GuestFile::File) return ret(c, std::uint32_t(EOF));
    FileScanSource src(f->fp);
    ret(c, std::uint32_t(guest_vscan(p, src, a.u(1), a.at(2))));
}

// ---- descriptors ----
void h_open(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string path = p.str(a.u(0));
    const std::uint32_t flags = a.u(1);
    Machine& m = p.machine();
    if (auto dev = m.open_device(p, path)) {
        GuestFd fd;
        fd.kind = GuestFd::Device;
        fd.dev = dev;
        return ret(c, std::uint32_t(p.os().new_fd(fd)));
    }
    int hflags = 0;
    switch (flags & 3) {
    case 0: hflags = O_RDONLY; break;
    case L_O_WRONLY: hflags = O_WRONLY; break;
    default: hflags = O_RDWR; break;
    }
    if (flags & L_O_CREAT) hflags |= O_CREAT;
    if (flags & L_O_TRUNC) hflags |= O_TRUNC;
    if (flags & L_O_APPEND) hflags |= O_APPEND;
    const bool writes = (flags & 3) != 0;
    const std::string host = writes ? m.map_write(path) : m.map_read(path);
    const int h = host.empty() ? -1 : ::open(host.c_str(), hflags, 0644);
    if (h < 0) {
        note_once("open", p.name() + ": " + path + " (not available)");
        return ret(c, std::uint32_t(-1));
    }
    GuestFd fd;
    fd.kind = GuestFd::File;
    fd.host = h;
    ret(c, std::uint32_t(p.os().new_fd(fd)));
}

void h_close(Cpu& c) {
    Process& p = proc(c);
    const int n = Args{c}.i(0);
    GuestFd* fd = p.os().fd(n);
    if (!fd) return ret(c, std::uint32_t(-1));
    if (fd->kind == GuestFd::File) ::close(fd->host);
    if (fd->dev) fd->dev->close();
    if (n > 2) *fd = GuestFd{};
    ret(c, 0);
}

void h_read(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFd* fd = p.os().fd(a.i(0));
    if (!fd) return ret(c, std::uint32_t(-1));
    switch (fd->kind) {
    case GuestFd::File: return ret(c, std::uint32_t(::read(fd->host, p.host(a.u(1)), a.u(2))));
    case GuestFd::Device: return ret(c, std::uint32_t(fd->dev->read(p, a.u(1), a.u(2))));
    case GuestFd::Stdin:
        if (p.os().stdin_dev) return ret(c, std::uint32_t(p.os().stdin_dev->read(p, a.u(1), a.u(2))));
        return ret(c, 0);  // the console keyboard: nothing typed
    default: return ret(c, std::uint32_t(-1));
    }
}

void h_write(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFd* fd = p.os().fd(a.i(0));
    if (!fd) return ret(c, std::uint32_t(-1));
    switch (fd->kind) {
    case GuestFd::File: return ret(c, std::uint32_t(::write(fd->host, p.host(a.u(1)), a.u(2))));
    case GuestFd::Device: return ret(c, std::uint32_t(fd->dev->write(p, a.u(1), a.u(2))));
    case GuestFd::Stdout:
        if (p.machine().config().stdout_echo)
            logf("[%s] %.*s", p.name().c_str(), int(a.u(2)), static_cast<char*>(p.host(a.u(1))));
        return ret(c, a.u(2));
    default: return ret(c, std::uint32_t(-1));
    }
}

void h_ioctl(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    GuestFd* fd = p.os().fd(a.i(0));
    if (!fd) return ret(c, std::uint32_t(-1));
    if (fd->kind == GuestFd::Device) return ret(c, std::uint32_t(fd->dev->ioctl(p, a.u(1), a.u(2))));
    char buf[64];
    std::snprintf(buf, sizeof buf, "ioctl 0x%08X on fd %d", a.u(1), a.i(0));
    note_once("ioctl", p.name() + ": " + buf);
    ret(c, 0);
}

// select(nfds, readfds, writefds, exceptfds, timeout): stvsnd waits on its
// stdin pipe with a timeout. fd_set is 32 words of bits.
void h_select(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const int nfds = a.i(0);
    const std::uint32_t rd = a.u(1), wr = a.u(2), tv = a.u(4);
    long timeout_us = -1;
    if (tv) timeout_us = long(p.u32(tv)) * 1000000L + long(p.u32(tv + 4));
    int ready = 0;
    if (rd) {
        std::uint32_t bits = p.u32(rd);
        std::uint32_t out = 0;
        for (int fdn = 0; fdn < nfds && fdn < 32; ++fdn) {
            if (!(bits & (1u << fdn))) continue;
            std::shared_ptr<Device> dev;
            if (fdn == 0 && p.os().stdin_dev) dev = p.os().stdin_dev;
            else if (GuestFd* fd = p.os().fd(fdn); fd && fd->dev) dev = fd->dev;
            if (!dev || dev->wait_readable(timeout_us)) {
                out |= 1u << fdn;
                ++ready;
            }
            timeout_us = 0;
        }
        p.set_u32(rd, out);
    }
    if (wr) {
        std::uint32_t bits = p.u32(wr);
        p.set_u32(wr, bits);
        for (int fdn = 0; fdn < 32; ++fdn)
            if (bits & (1u << fdn)) ++ready;
    }
    if (a.u(3)) p.set_u32(a.u(3), 0);
    if (!ready && timeout_us > 0) usleep(useconds_t(std::min<long>(timeout_us, 100000)));
    ret(c, std::uint32_t(ready));
}

// __xstat(version, path, struct stat*): glibc 2.2's i386 struct stat.
void h_xstat(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string path = p.str(a.u(1));
    const std::string host = p.machine().map_read(path);
    struct stat st;
    if (host.empty() || ::stat(host.c_str(), &st) != 0) return ret(c, std::uint32_t(-1));
    const std::uint32_t b = a.u(2);
    std::memset(p.host(b), 0, 88);
    const std::uint32_t mode = S_ISDIR(st.st_mode) ? (0040000u | 0755u) : (0100000u | 0644u);
    p.set_u32(b + 12, std::uint32_t(st.st_ino));
    p.set_u32(b + 16, mode);
    p.set_u32(b + 20, 1);
    p.set_u32(b + 44, std::uint32_t(st.st_size));
    p.set_u32(b + 48, 4096);
    p.set_u32(b + 52, std::uint32_t((st.st_size + 511) / 512));
    p.set_u32(b + 56, std::uint32_t(st.st_atime));
    p.set_u32(b + 64, std::uint32_t(st.st_mtime));
    p.set_u32(b + 72, std::uint32_t(st.st_ctime));
    ret(c, 0);
}

void h_popen(Cpu& c) {
    Args a{c};
    Process& p = proc(c);
    const std::string cmd = p.str(a.u(0)), mode = p.str(a.u(1));
    auto dev = p.machine().popen(p, cmd);
    if (!dev) {
        note_once("popen", p.name() + ": " + cmd);
        return ret(c, 0);
    }
    GuestFile f;
    f.kind = GuestFile::Device;
    f.dev = dev;
    f.path = cmd;
    ret(c, p.os().new_file(p, std::move(f)));
}

}  // namespace

void register_stdio_imports() {
    register_imports({
        {"fopen", h_fopen},
        {"fclose", h_fclose},
        {"fread", h_fread},
        {"fwrite", h_fwrite},
        {"fprintf", h_fprintf},
        {"fputc", h_fputc},
        {"fgetc", h_fgetc},
        {"fgets", h_fgets},
        {"feof", h_feof},
        {"fflush", h_fflush},
        {"fseek", h_fseek},
        {"ftell", h_ftell},
        {"fscanf", h_fscanf},
        {"open", h_open},
        {"close", h_close},
        {"read", h_read},
        {"write", h_write},
        {"ioctl", h_ioctl},
        {"select", h_select},
        {"__xstat", h_xstat},
        {"popen", h_popen},
    });
}

}

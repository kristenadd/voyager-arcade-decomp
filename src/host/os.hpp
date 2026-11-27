#pragma once
// The slice of Linux a guest program sees: stdio FILEs, file descriptors,
// devices, the environment. Paths are mapped by the Machine: /data/stv and
// the working folder read from the player's game folder; /var/stv and every
// file opened for writing live in the port's container folder.
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace voy::host {

class Process;
class Device;

struct GuestFile {
    enum Kind { File, Stdout, Device } kind = File;
    FILE* fp = nullptr;                    // File
    std::shared_ptr<host::Device> dev;    // Device (the stvsnd pipe)
    bool eof = false, error = false;
    std::string path;
};

struct GuestFd {
    enum Kind { None, File, Stdin, Stdout, Device } kind = None;
    int host = -1;                         // File
    std::shared_ptr<host::Device> dev;    // Device
};

// A character device or pipe end (/dev/mjf, /dev/dsp, the stvsnd pipe).
class Device {
public:
    virtual ~Device() = default;
    virtual int read(Process& p, std::uint32_t buf, std::uint32_t n) { (void)p; (void)buf; (void)n; return 0; }
    virtual int write(Process& p, std::uint32_t buf, std::uint32_t n) { (void)p; (void)buf; return int(n); }
    virtual int ioctl(Process& p, std::uint32_t request, std::uint32_t arg) { (void)p; (void)request; (void)arg; return 0; }
    // For select(): whether a read would not block; waits up to `timeout_us` (negative: forever).
    virtual bool wait_readable(long timeout_us) { (void)timeout_us; return true; }
    virtual void close() {}
};

struct OsState {
    std::map<std::uint32_t, GuestFile> files;  // by guest FILE* (a host-made guest object)
    std::vector<GuestFd> fds;
    std::map<std::string, std::string> env;
    std::map<std::string, std::uint32_t> env_strings;
    std::uint32_t stdout_ptr = 0;
    std::shared_ptr<Device> stdin_dev;  // what fd 0 reads (stvsnd: the pipe from trek)
    // glibc random_r TYPE_3 state for rand / srand.
    std::int32_t rand_r[34];
    int rand_f = 3, rand_b = 0;
    bool rand_seeded = false;

    std::uint32_t stdout_file(Process& p);
    std::uint32_t new_file(Process& p, GuestFile f);
    GuestFile* file(std::uint32_t ptr);
    int new_fd(GuestFd fd);
    GuestFd* fd(int n);
    void close_all(Process& p);
    std::int32_t rand();
    void srand(std::uint32_t seed);
};

}

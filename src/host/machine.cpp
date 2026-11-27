#include "host/machine.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "gl/gl.hpp"
#include "host/fail.hpp"
#include "host/os.hpp"
#include "host/process.hpp"

namespace voy::host {

void register_libc_imports();
void register_stdio_imports();
void register_gl_imports();

namespace fs = std::filesystem;

// ---- Input ----
void Input::shot(int gun, int x, int y) {
    std::lock_guard<std::mutex> lock(m_);
    shots_.push_back({gun & 1, x, y});
}
void Input::aim(int gun, int x, int y) {
    std::lock_guard<std::mutex> lock(m_);
    aim_x_[gun & 1] = x;
    aim_y_[gun & 1] = y;
}
void Input::set_button(Button b, bool down) {
    std::lock_guard<std::mutex> lock(m_);
    buttons_[b] = down;
}
void Input::coin(int chute) {
    std::lock_guard<std::mutex> lock(m_);
    ++coins_[chute & 1];
}
void Input::key(int ascii) {
    std::lock_guard<std::mutex> lock(m_);
    keys_.push_back(ascii);
}
Input::Snapshot Input::take() {
    std::lock_guard<std::mutex> lock(m_);
    Snapshot s;
    s.shots.swap(shots_);
    for (int i = 0; i < Count; ++i) s.buttons[i] = buttons_[i];
    s.coins[0] = coins_[0];
    s.coins[1] = coins_[1];
    for (int g = 0; g < 2; ++g) {
        s.aim_x[g] = aim_x_[g];
        s.aim_y[g] = aim_y_[g];
    }
    return s;
}
std::vector<int> Input::take_keys() {
    std::lock_guard<std::mutex> lock(m_);
    std::vector<int> k;
    k.swap(keys_);
    return k;
}

// ---- FrameClock ----
namespace {
std::uint64_t now_ns() {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count());
}
constexpr std::uint64_t kFrameNs = 1000000000ull / 60;
}  // namespace

FrameClock::FrameClock() : start_ns_(now_ns()) {}

namespace {
std::atomic<FrameClock*> g_clock{nullptr};
}

std::uint64_t FrameClock::ns() {
    if (virtual_) return std::uint64_t(virtual_frame_.load()) * kFrameNs + 1000 * (++virtual_reads_ % 16000);
    return now_ns() - start_ns_;
}

std::uint64_t clock_ns() {
    FrameClock* c = g_clock.load();
    return c ? c->ns() : now_ns();
}

std::uint32_t FrameClock::frame() const {
    if (virtual_) return virtual_frame_;
    return std::uint32_t((now_ns() - start_ns_) / kFrameNs);
}

int FrameClock::wait_periods(int periods) {
    if (virtual_) {
        virtual_frame_ += std::uint32_t(periods);
        last_exit_ = virtual_frame_;
        return periods;
    }
    std::uint32_t f = frame();
    if (f - last_exit_ > 30) last_exit_ = f - std::uint32_t(periods);  // after a stall: no catch-up burst
    const std::uint32_t target = last_exit_ + std::uint32_t(periods);
    while (std::int32_t(f - target) < 0) {
        const std::uint64_t due = start_ns_ + std::uint64_t(target) * kFrameNs;
        const std::uint64_t n = now_ns();
        if (due > n) std::this_thread::sleep_for(std::chrono::nanoseconds(due - n));
        f = frame();
    }
    const int passed = int(f - last_exit_);
    last_exit_ = f;
    return passed;
}

// ---- Machine ----
Machine::Machine(Config config, Programs programs)
    : config_(std::move(config)), programs_(programs), gl_(new gl::Context) {
    static bool registered = false;
    if (!registered) {
        register_libc_imports();
        register_stdio_imports();
        register_gl_imports();
        registered = true;
    }
    clock_.set_virtual(config_.headless);
    g_clock = &clock_;
    std::error_code ec;
    fs::create_directories(config_.container, ec);
    mjf_ = make_megajamma(*this);
    dsp_ = make_dsp(*this);
}

Machine::~Machine() { stop_sound(); }

namespace {
bool exists(const std::string& p, bool* is_dir) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}
std::string strip_dot(std::string p) {
    while (p.rfind("./", 0) == 0) p = p.substr(2);
    return p;
}
}  // namespace

std::string Machine::map_read(const std::string& path, bool* is_dir) const {
    if (path.rfind("/data/stv/", 0) == 0) {
        const std::string rest = path.substr(10);
        const std::string c = config_.container + "/data/stv/" + rest;
        if (exists(c, is_dir)) return c;
        const std::string h = config_.content + "/stv/" + rest;
        return exists(h, is_dir) ? h : std::string();
    }
    if (path.rfind("/var/stv/", 0) == 0) {
        const std::string h = config_.container + "/" + path.substr(9);
        return exists(h, is_dir) ? h : std::string();
    }
    if (!path.empty() && path[0] == '/') return std::string();  // /proc, /dev, ...: not here
    const std::string rel = strip_dot(path);
    const std::string c = config_.container + "/" + rel;
    if (exists(c, is_dir)) return c;
    const std::string h = config_.content + "/" + rel;
    return exists(h, is_dir) ? h : std::string();
}

std::string Machine::map_write(const std::string& path) const {
    std::string rel;
    if (path.rfind("/var/stv/", 0) == 0) rel = path.substr(9);
    else if (path.rfind("/data/stv/", 0) == 0) rel = "data/stv/" + path.substr(10);
    else if (!path.empty() && path[0] == '/') rel = "root" + path;
    else rel = strip_dot(path);
    const std::string h = config_.container + "/" + rel;
    std::error_code ec;
    fs::create_directories(fs::path(h).parent_path(), ec);
    return h;
}

std::shared_ptr<Device> Machine::open_device(Process& p, const std::string& path) {
    if (path == "/dev/mjf") return mjf_;
    if (path == "/dev/dsp") return dsp_;
    (void)p;
    return nullptr;
}

std::shared_ptr<Device> Machine::popen(Process& p, const std::string& command) {
    if (command.rfind("./stvsnd", 0) != 0) return nullptr;
    (void)p;
    stop_sound();
    sound_pipe_ = make_pipe();
    if (config_.sound && !programs_.stvsnd.empty()) start_sound();
    return sound_pipe_;
}

void Machine::start_sound() {
    sound_ = std::make_unique<Process>("stvsnd", *this);
    std::string err;
    if (!sound_->load(config_.content + "/stvsnd", programs_.stvsnd, &err)) {
        logf("[stvsnd] cannot load: %s\n", err.c_str());
        sound_.reset();
        return;
    }
    sound_->os().stdin_dev = sound_pipe_;
    Process* sp = sound_.get();
    sound_thread_ = std::thread([sp] {
        pthread_setname_np("stvsnd");
        const int code = sp->run({"./stvsnd", "/data/stv/arom/"});
        if (code != 0 && code != -3) logf("[stvsnd] exited with %d\n", code);
    });
}

void Machine::stop_sound() {
    if (sound_) sound_->request_stop(-3);
    if (sound_pipe_) sound_pipe_->close();
    if (dsp_) {
        // The sound program may be blocked writing audio; closing wakes it.
    }
    if (sound_thread_.joinable()) sound_thread_.join();
    sound_.reset();
}

Machine::AudioFormat Machine::audio_format() const { return dsp_format(*dsp_); }

void Machine::pull_audio(std::int16_t* out, int frames, int rate) { dsp_pull(*dsp_, out, frames, rate); }

void Machine::count_frame() { ++frames_drawn_; }

void Machine::request_quit() {
    quit_ = true;
    std::lock_guard<std::mutex> lock(current_m_);
    if (current_) current_->request_stop(0);
    if (sound_pipe_) sound_pipe_->close();
}

void Machine::copy_default_files() {
    for (const char* name : {"stv.settings", "stv.audits", "stv.hiscores", "audits.dat"}) {
        const std::string dst = config_.container + "/" + name;
        if (exists(dst, nullptr)) continue;
        std::error_code ec;
        fs::copy_file(config_.content + "/" + name, dst, ec);
    }
}

int Machine::run_program(const char* name, std::span<const recomp::GuestEntry> entries) {
    Process p(name, *this);
    std::string err;
    if (!p.load(config_.content + "/" + name, entries, &err)) {
        logf("[%s] cannot load: %s\n", name, err.c_str());
        return -1;
    }
    gl_->reset();
    {
        std::lock_guard<std::mutex> lock(current_m_);
        current_ = &p;
    }
    const int code = p.run({std::string("./") + name});
    {
        std::lock_guard<std::mutex> lock(current_m_);
        current_ = nullptr;
    }
    return code;
}

// ssv: run the game; a normal exit (or the test switch, status 0x44) runs the
// service menu, then the game again; 'x' (0x51) quits; a crash restarts it.
int Machine::run() {
    copy_default_files();
    int code = 0, crashes = 0;
    while (!quit_) {
        code = run_program("trek", programs_.trek);
        stop_sound();
        if (quit_) break;
        if (code == 0x51) break;
        if (code == -2 || code == -1) break;  // fail closed: report, do not loop
        if (code == 0 || code == 0x44) {
            if (programs_.stvserv.empty()) continue;
            code = run_program("stvserv", programs_.stvserv);
            stop_sound();
            if (code == -2 || code == -1) break;
            continue;
        }
        if (++crashes > 3) break;
        logf("[ssv] trek exited with %d; restarting it\n", code);
    }
    return code;
}

}

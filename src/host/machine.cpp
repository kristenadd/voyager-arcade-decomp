#include "host/machine.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cctype>
#include <filesystem>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>

#include "gl/gl.hpp"
#include "host/fail.hpp"
#include "host/os.hpp"
#include "host/process.hpp"
#include "recomp/x86ops.hpp"

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
    load_script();
}

void Machine::load_script() {
    if (config_.input_script.empty()) return;
    std::ifstream f(config_.input_script);
    if (!f) {
        logf("[script] cannot read %s\n", config_.input_script.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        ScriptEvent e;
        if (!(ls >> e.frame >> e.cmd)) continue;
        std::string a;
        while (ls >> a) e.args.push_back(a);
        if (e.cmd == "tap-start" && !e.args.empty()) {
            script_.push_back({e.frame, "start", {e.args[0], "down"}});
            script_.push_back({e.frame + 6, "start", {e.args[0], "up"}});
            continue;
        }
        script_.push_back(std::move(e));
    }
    std::stable_sort(script_.begin(), script_.end(), [](const ScriptEvent& x, const ScriptEvent& y) { return x.frame < y.frame; });
}

void Machine::run_script() {
    while (script_next_ < script_.size() && script_[script_next_].frame <= frames_drawn_) {
        const auto& e = script_[script_next_++];
        auto arg = [&](std::size_t i) { return i < e.args.size() ? e.args[i] : std::string(); };
        auto num = [&](std::size_t i) { return std::atoi(arg(i).c_str()); };
        if (e.cmd == "coin") input_.coin(num(0));
        else if (e.cmd == "start") input_.set_button(num(0) == 2 ? Input::Start2 : Input::Start1, arg(1) != "up");
        else if (e.cmd == "shot") input_.shot(num(0), num(1), num(2));
        else if (e.cmd == "reload") input_.shot(num(0), -1, -1);
        else if (e.cmd == "test") input_.set_button(Input::Test, arg(0) != "up");
        else if (e.cmd == "key") input_.key(arg(0).empty() ? 0 : arg(0)[0]);
        else if (e.cmd == "quit") request_quit();
        else logf("[script] unknown command %s\n", e.cmd.c_str());
        logf("[script] frame %llu: %s\n", (unsigned long long)frames_drawn_.load(), e.cmd.c_str());
    }
}

Machine::~Machine() { stop_sound(); }

namespace {

// The game asks for names whose case differs from the files in the dump
// (Hlk_... for hlk_...). Case-insensitive volumes hide that; iOS's is case
// sensitive, so a missing path is looked up again one component at a time,
// ignoring case, from cached directory listings.
std::mutex g_dir_m;
std::unordered_map<std::string, std::unordered_map<std::string, std::string>> g_dirs;

std::string lower(std::string s) {
    for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

const std::unordered_map<std::string, std::string>& listing(const std::string& dir) {
    auto it = g_dirs.find(dir);
    if (it != g_dirs.end()) return it->second;
    auto& m = g_dirs[dir];
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        m.emplace(lower(n), n);
    }
    return m;
}

bool exists(const std::string& p, bool* is_dir) {
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return false;
    if (is_dir) *is_dir = S_ISDIR(st.st_mode);
    return true;
}

// `rel` under `root`, matching each component's case to what is on disk.
std::string resolve(const std::string& root, const std::string& rel, bool* is_dir) {
    const std::string direct = root + "/" + rel;
    if (exists(direct, is_dir)) return direct;
    std::lock_guard<std::mutex> lock(g_dir_m);
    std::string cur = root;
    std::size_t pos = 0;
    while (pos <= rel.size()) {
        const std::size_t slash = rel.find('/', pos);
        const std::string part = rel.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!part.empty() && part != ".") {
            const auto& l = listing(cur);
            auto it = l.find(lower(part));
            if (it == l.end()) return {};
            cur += "/" + it->second;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return exists(cur, is_dir) ? cur : std::string();
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
        return resolve(config_.content + "/stv", rest, is_dir);
    }
    if (path.rfind("/var/stv/", 0) == 0) {
        const std::string h = config_.container + "/" + path.substr(9);
        return exists(h, is_dir) ? h : std::string();
    }
    if (!path.empty() && path[0] == '/') return std::string();  // /proc, /dev, ...: not here
    const std::string rel = strip_dot(path);
    const std::string c = config_.container + "/" + rel;
    if (exists(c, is_dir)) return c;
    return resolve(config_.content, rel, is_dir);
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

bool Machine::player_playing(int n) {
    std::lock_guard<std::mutex> lock(current_m_);
    if (!current_ || current_->name() != "trek") return false;
    const std::uint32_t players = current_->sym("g_Players");
    if (!players) return false;
    return recomp::ld16(current_->mem(), players + std::uint32_t(n & 1) * 55460u + 0x734u) != 0;
}

void Machine::count_frame() {
    ++frames_drawn_;
    if (script_next_ < script_.size()) run_script();
}

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

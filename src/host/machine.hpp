#pragma once
// The cabinet: the player's game folder, the port's container folder, the
// devices the programs open, the input the host feeds in, the 60 Hz vblank,
// the OpenGL state, and the supervisor that runs trek, the service menu
// (stvserv) and the sound program (stvsnd) as ssv did.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "recomp/dispatch.hpp"

namespace voy::gl {
class Context;
}

namespace voy::host {

class Process;
class Device;

struct Config {
    std::string content;    // the player's game folder (read only)
    std::string container;  // writable: settings, audits, scores, logs
    bool headless = false;
    bool sound = true;
    bool stdout_echo = false;     // the programs' printf output to stderr
    bool free_play = false;       // forced free play (iOS)
    int max_frames = 0;           // stop after this many drawn frames (tests)
    std::string input_script;     // scripted input (tests): see input.hpp
};

// Translated code for each program (generated tables, per build).
struct Programs {
    std::span<const recomp::GuestEntry> trek, stvsnd, stvserv;
};

// Host input, written by the UI thread, read by the game thread.
struct GunShot {
    int gun;      // 0 = P1, 1 = P2
    int x, y;     // game coordinates (512 x 384); -1, -1 = off screen (reload)
};

class Input {
public:
    // A trigger pull at a point in game coordinates (or off screen).
    void shot(int gun, int x, int y);
    void aim(int gun, int x, int y);
    // Buttons: held state. coin is edge counted.
    enum Button { Start1, Start2, Service, Test, VolUp, VolDown, Count };
    void set_button(Button b, bool down);
    void coin(int chute);
    void key(int ascii);  // a debug key to the game's keyboard handler

    struct Snapshot {
        std::vector<GunShot> shots;
        bool buttons[Count] = {};
        std::uint32_t coins[2] = {};
        int aim_x[2] = {256, 256}, aim_y[2] = {192, 192};
    };
    // Takes the pending shots, and the current buttons and coin counts.
    Snapshot take();
    std::vector<int> take_keys();
    std::uint32_t coin_count(int chute) const { return coins_[chute]; }

private:
    mutable std::mutex m_;
    std::vector<GunShot> shots_;
    std::vector<int> keys_;
    bool buttons_[Count] = {};
    std::uint32_t coins_[2] = {};
    int aim_x_[2] = {256, 256}, aim_y_[2] = {192, 192};
};

// The vblank: 60 per second from the host clock.
class FrameClock {
public:
    FrameClock();
    std::uint32_t frame() const;
    // Waits until frame() has advanced by at least `periods` since the last
    // call; returns how many frames passed.
    int wait_periods(int periods);
    // Virtual time (headless tests): frame() advances only by wait_periods.
    void set_virtual(bool v) { virtual_ = v; }
    // Nanoseconds since start on this clock (the guest's time stamp counter):
    // in virtual time, the vblank time plus 1 us per reading, so it still moves.
    std::uint64_t ns();

private:
    std::uint64_t start_ns_;
    std::uint32_t last_exit_ = 0;
    bool virtual_ = false;
    std::atomic<std::uint32_t> virtual_frame_{0};
    std::atomic<std::uint64_t> virtual_reads_{0};
};

class Machine {
public:
    Machine(Config config, Programs programs);
    ~Machine();

    const Config& config() const { return config_; }
    // Guest path → host path: for reading (container first, then the game
    // folder) and for writing (always the container).
    std::string map_read(const std::string& guest_path, bool* is_dir = nullptr) const;
    std::string map_write(const std::string& guest_path) const;
    // Opens a device path (/dev/mjf, /dev/dsp); nullptr when not a device.
    std::shared_ptr<Device> open_device(Process& p, const std::string& path);
    // popen(): the sound program. Returns the write end of its stdin pipe.
    std::shared_ptr<Device> popen(Process& p, const std::string& command);

    Input& input() { return input_; }
    FrameClock& clock() { return clock_; }
    gl::Context& gl() { return *gl_; }

    // Runs the cabinet until quit: trek, the service menu after a normal exit,
    // trek again after a crash. Returns the last exit status.
    int run();
    void request_quit();
    bool quitting() const { return quit_; }

    // The audio the sound program writes to /dev/dsp: 16-bit stereo at the
    // rate it asked for. The platform's audio callback pulls it.
    struct AudioFormat {
        int rate = 0, channels = 0, bits = 0;
    };
    AudioFormat audio_format() const;
    // Fills `out` with interleaved stereo int16 frames at `rate`; silence when there are none.
    void pull_audio(std::int16_t* out, int frames, int rate);

    // Frames drawn by the game since start (glutSwapBuffers).
    std::uint64_t frames_drawn() const { return frames_drawn_; }
    void count_frame();

private:
    int run_program(const char* name, std::span<const recomp::GuestEntry> entries);
    void start_sound();
    void stop_sound();
    void copy_default_files();

    Config config_;
    Programs programs_;
    Input input_;
    FrameClock clock_;
    std::unique_ptr<gl::Context> gl_;
    std::shared_ptr<Device> mjf_, dsp_, sound_pipe_;
    std::unique_ptr<Process> sound_;
    std::thread sound_thread_;
    std::atomic<bool> quit_{false};
    std::atomic<std::uint64_t> frames_drawn_{0};
    Process* current_ = nullptr;
    std::mutex current_m_;
};

// The running machine's clock in nanoseconds (guest rdtsc).
std::uint64_t clock_ns();

// Device constructors (devices.cpp).
std::shared_ptr<Device> make_megajamma(Machine& m);
std::shared_ptr<Device> make_dsp(Machine& m);
std::shared_ptr<Device> make_pipe();
// The dsp device's audio, for Machine::pull_audio.
Machine::AudioFormat dsp_format(Device& dsp);
void dsp_pull(Device& dsp, std::int16_t* out, int frames, int out_rate);

}

#pragma once
// The cabinet on a game thread, for the Mac and iOS apps: runs the Machine
// with a large host stack, keeps the newest finished frame for the display
// link, and turns pointer, keyboard and controller input into gun shots and
// buttons.
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "gl/gl.hpp"
#include "host/machine.hpp"

namespace voy::platform {

class GameHost : public gl::FrameSink {
public:
    explicit GameHost(host::Config config);
    ~GameHost() override;

    void start();
    void stop();
    bool running() const { return running_; }
    int exit_code() const { return exit_code_; }

    host::Machine& machine() { return *machine_; }
    void submit(std::shared_ptr<const gl::Frame> frame) override;
    // The newest frame if it is newer than the last one taken, else null.
    std::shared_ptr<const gl::Frame> take_new();

    // Pointer input in game coordinates (512 x 384, y down); outside the
    // picture is an off-screen shot (reload / remodulate).
    void pointer_shot(int gun, float gx, float gy, bool inside);

private:
    std::unique_ptr<host::Machine> machine_;
    std::mutex m_;
    std::shared_ptr<const gl::Frame> latest_;
    std::uint64_t taken_ = ~0ull;
    pthread_t thread_{};
    bool started_ = false;
    std::atomic<bool> running_{false};
    std::atomic<int> exit_code_{0};
};

// Maps a point in drawable pixels to game coordinates, given the rect the
// game was presented in (x0, y0, x1, y1). Returns whether it is inside.
bool to_game(const float rect[4], float px, float py, float* gx, float* gy);

}

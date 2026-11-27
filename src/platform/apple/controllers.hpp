#pragma once
// Game controllers as light guns.
//
// Gamepads: the left stick (or right stick) moves a crosshair, A / right
// trigger fires (held: repeats), B / left trigger / right shoulder reloads
// and remodulates (an off-screen shot), Menu is start, Options / View inserts
// a coin. The first gamepad is player 1 (red crosshair), the second player 2
// (blue crosshair).
//
// Spatial controllers (PlayStation VR2 Sense, on visionOS) are light guns:
// the right-hand one is player 1 and the left-hand one player 2 (a lone one
// is player 1), and gamepads take the places left. Once accessory tracking is
// allowed, the crosshair follows where the controller points
// (platform/visionos/aim.swift) and the trigger shoots there; pointed off the
// picture, the trigger reloads. Untracked, the thumbstick moves the crosshair.
// Grip reloads, Cross / Square (and Options / Create) start, Circle /
// Triangle insert a coin.
#include "gpu/renderer.hpp"

#include <functional>

namespace voy::host {
class Machine;
}

namespace voy::platform {

// VOY_INPUT_LOG=1: touches, controllers and their presses on stderr.
void input_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

class Controllers {
public:
    Controllers();
    // Called once per display frame.
    void poll(host::Machine& m, double dt);
    // The crosshairs to draw (hidden for players without a controller).
    void cursors(gpu::Renderer::Cursor out[2]) const;
    bool any_connected() const;
    // True once after a frame in which a stick moved or a controller button
    // was down.
    bool take_activity() { const bool a = activity_; activity_ = false; return a; }

    // visionOS turns controller presses into pinches on the view as well.
    // True when a controller button went down within `window` seconds of `t` (now_seconds()).
    bool pad_press_near(double t, double window) const;
    static double now_seconds();

    // visionOS: track where spatial controllers point on this UIWindow.
    void attach_window(void* ui_window);
    // Maps a point on the window (0..1 from the top left) to game
    // coordinates; returns whether it is on the picture.
    void set_window_to_game(std::function<bool(float fx, float fy, float* gx, float* gy)> f) {
        window_to_game_ = std::move(f);
    }

private:
    struct Player {
        float x = 256, y = 192;
        bool fire = false, reload = false, start = false, coin = false, trigger = false;
        double repeat = 0;
        bool connected = false, spatial = false, tracked = false;
    } p_[2];
    bool activity_ = false;
    double last_press_ = -1e9;
    unsigned long seen_ = ~0ul;  // controller count last logged
    std::function<bool(float, float, float*, float*)> window_to_game_;
};

}

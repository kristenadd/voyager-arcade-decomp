#pragma once
// Game controllers as light guns: the left stick (or right stick) moves a
// crosshair, A / right trigger fires (held: repeats), B / left trigger
// reloads and remodulates (an off-screen shot), Menu is start, Options /
// View inserts a coin. The first controller is player 1, the second player 2.
#include "gpu/renderer.hpp"

namespace voy::host {
class Machine;
}

namespace voy::platform {

class Controllers {
public:
    Controllers();
    // Called once per display frame.
    void poll(host::Machine& m, double dt);
    // The crosshairs to draw (hidden for players without a controller).
    void cursors(gpu::Renderer::Cursor out[2]) const;
    bool any_connected() const;
    // True once after a frame in which a stick moved or a button was down.
    bool take_activity() { const bool a = activity_; activity_ = false; return a; }

private:
    struct Player {
        float x = 256, y = 192;
        bool fire = false, reload = false, start = false, coin = false;
        double repeat = 0;
        bool connected = false;
    } p_[2];
    bool activity_ = false;
};

}

#include "platform/apple/controllers.hpp"

#import <GameController/GameController.h>

#include <cmath>

#include "host/machine.hpp"

namespace voy::platform {

Controllers::Controllers() {}

bool Controllers::any_connected() const { return p_[0].connected || p_[1].connected; }

void Controllers::poll(host::Machine& m, double dt) {
    NSArray<GCController*>* pads = GCController.controllers;
    for (int i = 0; i < 2; ++i) {
        Player& pl = p_[i];
        GCExtendedGamepad* g = i < int(pads.count) ? pads[NSUInteger(i)].extendedGamepad : nil;
        if (!g) {
            if (pl.connected) {
                m.input().set_button(i == 0 ? host::Input::Start1 : host::Input::Start2, false);
            }
            pl.connected = false;
            continue;
        }
        pl.connected = true;
        float sx = g.leftThumbstick.xAxis.value, sy = g.leftThumbstick.yAxis.value;
        if (std::fabs(g.rightThumbstick.xAxis.value) + std::fabs(g.rightThumbstick.yAxis.value) > std::fabs(sx) + std::fabs(sy)) {
            sx = g.rightThumbstick.xAxis.value;
            sy = g.rightThumbstick.yAxis.value;
        }
        const float dead = 0.12f;
        auto curve = [&](float v) {
            const float a = std::fabs(v);
            if (a < dead) return 0.0f;
            const float n = (a - dead) / (1 - dead);
            return std::copysign(n * n * 520.0f, v);  // game pixels per second at full tilt
        };
        pl.x = std::fmin(511.0f, std::fmax(0.0f, pl.x + curve(sx) * float(dt)));
        pl.y = std::fmin(383.0f, std::fmax(0.0f, pl.y - curve(sy) * float(dt)));
        m.input().aim(i, int(pl.x), int(pl.y));
        const bool fire = g.buttonA.pressed || g.rightTrigger.value > 0.4f;
        const bool reload = g.buttonB.pressed || g.leftTrigger.value > 0.4f || g.rightShoulder.pressed;
        const bool start = g.buttonMenu.pressed;
        const bool coin = g.buttonOptions && g.buttonOptions.pressed;
        if (fire && !pl.fire) {
            m.input().shot(i, int(pl.x), int(pl.y));
            pl.repeat = 0.18;
        } else if (fire) {
            pl.repeat -= dt;
            if (pl.repeat <= 0) {
                m.input().shot(i, int(pl.x), int(pl.y));
                pl.repeat = 0.12;
            }
        }
        if (reload && !pl.reload) m.input().shot(i, -1, -1);
        if (start != pl.start) m.input().set_button(i == 0 ? host::Input::Start1 : host::Input::Start2, start);
        if (coin && !pl.coin) m.input().coin(i);
        pl.fire = fire;
        pl.reload = reload;
        pl.start = start;
        pl.coin = coin;
    }
}

void Controllers::cursors(gpu::Renderer::Cursor out[2]) const {
    for (int i = 0; i < 2; ++i) {
        out[i].visible = p_[i].connected;
        out[i].x = p_[i].x;
        out[i].y = p_[i].y;
        out[i].r = i == 0 ? 1.0f : 0.3f;
        out[i].g = i == 0 ? 0.85f : 0.8f;
        out[i].b = i == 0 ? 0.2f : 1.0f;
    }
}

}

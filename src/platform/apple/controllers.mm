#include "platform/apple/controllers.hpp"

#import <GameController/GameController.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#include "host/machine.hpp"

#if TARGET_OS_VISION
// platform/visionos/aim.swift: where each spatial controller points.
extern "C" void voy_vision_aim_attach(void* window);
extern "C" int voy_vision_aim_poll(const void** controllers, float* values, int max);
#endif

namespace voy::platform {

namespace {

struct Reading {
    float sx = 0, sy = 0;
    bool fire = false, reload = false, start = false, coin = false, trigger = false;
};

void take_stick(Reading& r, float x, float y) {
    if (std::fabs(x) + std::fabs(y) > std::fabs(r.sx) + std::fabs(r.sy)) {
        r.sx = x;
        r.sy = y;
    }
}

void read_gamepad(GCExtendedGamepad* g, Reading& r) {
    take_stick(r, g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value);
    take_stick(r, g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value);
    r.fire = g.buttonA.pressed || g.rightTrigger.value > 0.4f;
    r.reload = g.buttonB.pressed || g.leftTrigger.value > 0.4f || g.rightShoulder.pressed;
    r.start = g.buttonMenu.pressed;
    r.coin = g.buttonOptions && g.buttonOptions.pressed;
}

bool is_spatial(GCController* c) {
    if (@available(macOS 26.0, iOS 26.0, visionOS 26.0, *))
        return [c.productCategory isEqualToString:GCProductCategorySpatialController];
    return false;
}

// One hand of a PlayStation VR2 Sense pair (each hand is its own controller).
void read_spatial(GCController* c, Reading& r) {
    if (@available(macOS 26.0, iOS 26.0, visionOS 26.0, *)) {
        GCPhysicalInputProfile* p = c.physicalInputProfile;
        if (GCControllerDirectionPad* st = p.dpads[GCInputThumbstick]) take_stick(r, st.xAxis.value, st.yAxis.value);
        auto down = [&](NSString* name) {
            GCControllerButtonInput* b = p.buttons[name];
            return b != nil && b.pressed;
        };
        r.trigger = r.trigger || down(GCInputTrigger);
        // Cross (right hand) and Square (left) start, Circle (right) and
        // Triangle (left) insert a coin, grip reloads.
        r.reload = r.reload || down(GCInputGripButton);
        r.start = r.start || down(GCInputButtonA) || down(GCInputButtonX) || down(GCInputButtonMenu) ||
                  down(GCInputButtonOptions);
        r.coin = r.coin || down(GCInputButtonB) || down(GCInputButtonY);
    }
}

}  // namespace

void input_log(const char* fmt, ...) {
    static const bool on = getenv("VOY_INPUT_LOG") != nullptr;
    if (!on) return;
    va_list a;
    va_start(a, fmt);
    std::fprintf(stderr, "[input] ");
    std::vfprintf(stderr, fmt, a);
    std::fprintf(stderr, "\n");
    va_end(a);
}

Controllers::Controllers() {}

double Controllers::now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool Controllers::any_connected() const { return p_[0].connected || p_[1].connected; }

void Controllers::attach_window(void* ui_window) {
#if TARGET_OS_VISION
    voy_vision_aim_attach(ui_window);
#endif
    (void)ui_window;
}

bool Controllers::pad_press_near(double t, double window) const { return std::fabs(t - last_press_) <= window; }

void Controllers::poll(host::Machine& m, double dt) {
    const double now = now_seconds();
    NSMutableArray<GCController*>* spatial = [NSMutableArray array];
    NSMutableArray<GCController*>* pads = [NSMutableArray array];
    for (GCController* c in GCController.controllers) {
        if (is_spatial(c)) [spatial addObject:c];
        else if (c.extendedGamepad) [pads addObject:c];
    }
    if (GCController.controllers.count != seen_) {
        seen_ = GCController.controllers.count;
        input_log("%lu controller(s)", seen_);
        for (GCController* c in GCController.controllers) {
            NSArray* names = [c.physicalInputProfile.elements.allKeys sortedArrayUsingSelector:@selector(compare:)];
            input_log("  %s / %s, spatial %d, extended %d: %s", c.vendorName.UTF8String ?: "?",
                      c.productCategory.UTF8String ?: "?", is_spatial(c) ? 1 : 0, c.extendedGamepad ? 1 : 0,
                      [names componentsJoinedByString:@", "].UTF8String);
            // With logging on, every button change the framework delivers
            // (whether or not the per-frame poll sees it).
            if (getenv("VOY_INPUT_LOG")) {
                NSString* name = c.vendorName ?: @"?";
                c.physicalInputProfile.valueDidChangeHandler = ^(GCPhysicalInputProfile*, GCControllerElement* e) {
                    if (![e isKindOfClass:GCControllerButtonInput.class]) return;  // sticks: see the player lines
                    GCControllerButtonInput* b = (GCControllerButtonInput*)e;
                    if (b.value > 0 && b.value < 1) return;  // a trigger on its way
                    input_log("event %s: %s %.2f", name.UTF8String, (e.localizedName ?: e.description).UTF8String, b.value);
                };
            }
        }
    }
    // Where each spatial controller points (visionOS): hand, tracked, x, y.
    struct Aim {
        int hand = 0;  // 0 unknown, 1 left, 2 right
        bool tracked = false;
        float fx = -1, fy = -1;  // 0..1 across the window
    };
    NSMapTable<GCController*, NSValue*>* aims = [NSMapTable weakToStrongObjectsMapTable];
#if TARGET_OS_VISION
    if (spatial.count) {
        constexpr int kMax = 4;
        const void* handles[kMax] = {};
        float values[kMax * 4] = {};
        const int n = voy_vision_aim_poll(handles, values, kMax);
        for (int k = 0; k < n; ++k) {
            Aim a{int(values[k * 4]), values[k * 4 + 1] != 0, values[k * 4 + 2], values[k * 4 + 3]};
            [aims setObject:[NSValue valueWithBytes:&a objCType:@encode(Aim)]
                     forKey:(__bridge GCController*)handles[k]];
        }
    }
#endif
    auto aim_of = [&](GCController* c) {
        Aim a;
        if (NSValue* v = [aims objectForKey:c]) [v getValue:&a size:sizeof a];
        return a;
    };
    // The right-hand spatial controller is player 1 and the left-hand one
    // player 2 (a lone one is player 1); gamepads take the places left.
    [spatial sortUsingComparator:^NSComparisonResult(GCController* a, GCController* b) {
        auto rank = [&](GCController* c) { const int h = aim_of(c).hand; return h == 2 ? 0 : h == 1 ? 2 : 1; };
        return rank(a) < rank(b) ? NSOrderedAscending : rank(a) > rank(b) ? NSOrderedDescending : NSOrderedSame;
    }];
    GCController* slots[2] = {nil, nil};
    int next = 0;
    for (GCController* c in spatial)
        if (next < 2) slots[next++] = c;
    for (GCController* c in pads)
        if (next < 2) slots[next++] = c;

    for (int i = 0; i < 2; ++i) {
        Player& pl = p_[i];
        const host::Input::Button start_button = i == 0 ? host::Input::Start1 : host::Input::Start2;
        if (!slots[i]) {
            if (pl.connected) m.input().set_button(start_button, false);
            pl = Player{pl.x, pl.y};
            continue;
        }
        GCController* c = slots[i];
        pl.connected = true;
        pl.spatial = is_spatial(c);
        // The pad's player LEDs and light bar (where it has them) match its crosshair.
        const GCControllerPlayerIndex index = i == 0 ? GCControllerPlayerIndex1 : GCControllerPlayerIndex2;
        if (c.playerIndex != index) {
            c.playerIndex = index;
            if (c.light) c.light.color = i == 0 ? [[GCColor alloc] initWithRed:1 green:0 blue:0]
                                                : [[GCColor alloc] initWithRed:0 green:0.3f blue:1];
        }
        Reading r;
        if (pl.spatial) read_spatial(c, r);
        else read_gamepad(c.extendedGamepad, r);

        // A tracked spatial controller points like a light gun: the crosshair
        // follows it, and pointing off the picture makes the trigger reload.
        bool off_picture = false;
        const Aim a = pl.spatial ? aim_of(c) : Aim{};
        if (a.tracked && window_to_game_) {
            float gx, gy;
            const bool inside = window_to_game_(a.fx, a.fy, &gx, &gy);
            if (inside) {
                if (std::fabs(gx - pl.x) + std::fabs(gy - pl.y) > 2) activity_ = true;
                pl.x = std::fmin(511.0f, std::fmax(0.0f, gx));
                pl.y = std::fmin(383.0f, std::fmax(0.0f, gy));
                r.sx = r.sy = 0;  // the stick does not fight the pointing
            }
            off_picture = !inside;
        }
        if (a.tracked != pl.tracked) input_log("player %d: %s", i + 1, a.tracked ? "pointing at the window" : "not pointing at the window");
        pl.tracked = a.tracked;

        const float dead = 0.12f;
        auto curve = [&](float v) {
            const float a = std::fabs(v);
            if (a < dead) return 0.0f;
            const float n = (a - dead) / (1 - dead);
            return std::copysign(n * n * 520.0f, v);  // game pixels per second at full tilt
        };
        pl.x = std::fmin(511.0f, std::fmax(0.0f, pl.x + curve(r.sx) * float(dt)));
        pl.y = std::fmin(383.0f, std::fmax(0.0f, pl.y - curve(r.sy) * float(dt)));
        m.input().aim(i, int(pl.x), int(pl.y));
        if (curve(r.sx) != 0 || curve(r.sy) != 0 || r.fire || r.reload || r.start || r.coin || r.trigger) activity_ = true;
        if ((r.fire && !pl.fire) || (r.reload && !pl.reload) || (r.start && !pl.start) || (r.coin && !pl.coin) ||
            (r.trigger && !pl.trigger))
            last_press_ = now;
        if (r.fire != pl.fire || r.reload != pl.reload || r.start != pl.start || r.coin != pl.coin || r.trigger != pl.trigger)
            input_log("player %d: fire %d reload %d start %d coin %d trigger %d stick %.2f %.2f", i + 1, r.fire, r.reload,
                      r.start, r.coin, r.trigger, r.sx, r.sy);

        if (r.fire && !pl.fire) {
            m.input().shot(i, int(pl.x), int(pl.y));
            pl.repeat = 0.18;
        } else if (r.fire) {
            pl.repeat -= dt;
            if (pl.repeat <= 0) {
                m.input().shot(i, int(pl.x), int(pl.y));
                pl.repeat = 0.12;
            }
        }
        // The spatial trigger: one shot per pull, where it points (or at the
        // crosshair the stick moves while it is not tracked); off the picture
        // it reloads.
        if (r.trigger && !pl.trigger) {
            if (off_picture) m.input().shot(i, -1, -1);
            else m.input().shot(i, int(pl.x), int(pl.y));
        }
        if (r.reload && !pl.reload) m.input().shot(i, -1, -1);
        if (r.start != pl.start) m.input().set_button(start_button, r.start);
        if (r.coin && !pl.coin) m.input().coin(i);
        pl.fire = r.fire;
        pl.reload = r.reload;
        pl.start = r.start;
        pl.coin = r.coin;
        pl.trigger = r.trigger;
    }
}

void Controllers::cursors(gpu::Renderer::Cursor out[2]) const {
    for (int i = 0; i < 2; ++i) {
        out[i].visible = p_[i].connected;
        out[i].x = p_[i].x;
        out[i].y = p_[i].y;
        // Player 1 red, player 2 blue.
        out[i].r = i == 0 ? 1.0f : 0.2f;
        out[i].g = i == 0 ? 0.15f : 0.5f;
        out[i].b = i == 0 ? 0.15f : 1.0f;
    }
}

}

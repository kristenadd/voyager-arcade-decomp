#include "host/glut.hpp"

#include <map>
#include <mutex>

#include "gl/gl.hpp"
#include "host/fail.hpp"
#include "host/machine.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

namespace {
std::mutex g_m;
std::map<Process*, GlutState> g_states;
}  // namespace

GlutState& glut_state(Process& p) {
    std::lock_guard<std::mutex> lock(g_m);
    return g_states[&p];
}

void glut_main_loop(Cpu& c) {
    Process& p = proc(c);
    Machine& m = p.machine();
    GlutState& g = glut_state(p);
    if (g.reshape) call_guest(c, g.reshape, {std::uint32_t(gl::kWidth), std::uint32_t(gl::kHeight)});
    if (g.visibility) call_guest(c, g.visibility, {1});  // GLUT_VISIBLE
    for (;;) {
        if (m.quitting()) p.exit(0);
        if (p.stop_requested()) throw GuestStop{p.stop_code(), "stop requested"};
        if (g.keyboard)
            for (int k : m.input().take_keys()) call_guest(c, g.keyboard, {std::uint32_t(k & 0xFF), 0, 0});
        if (g.idle) call_guest(c, g.idle, {});
        if (g.display && (g.redisplay || !g.idle)) {
            g.redisplay = false;
            call_guest(c, g.display, {});
        }
        if (m.config().max_frames && m.frames_drawn() >= std::uint64_t(m.config().max_frames)) m.request_quit();
    }
}

}

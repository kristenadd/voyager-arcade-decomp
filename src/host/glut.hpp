#pragma once
// GLUT's callbacks and main loop. The game hands control to glutMainLoop
// and never gets it back; the loop runs on the game thread, calls the idle
// and display callbacks, feeds keys to the keyboard callback, and unwinds
// the program (GuestStop) when the host asks it to stop.
#include <cstdint>

#include "recomp/cpu.hpp"

namespace voy::host {

class Process;

struct GlutState {
    std::uint32_t display = 0, idle = 0, keyboard = 0, special = 0, mouse = 0, reshape = 0, visibility = 0;
    bool redisplay = false;
};

GlutState& glut_state(Process& p);
[[noreturn]] void glut_main_loop(recomp::Cpu& c);

}

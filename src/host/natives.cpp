// Native replacements for guest functions (recomp/native.hpp).
#include "recomp/native.hpp"

#include "host/machine.hpp"
#include "host/process.hpp"

namespace voy::recomp {

using namespace host;

void native_verify_ib(Cpu& c) { ret(c, 1); }

void native_get_frame_number(Cpu& c) { ret(c, proc(c).machine().clock().frame()); }

void native_wait_for_vblank(Cpu& c) {
    Process& p = proc(c);
    const int period = Args{c}.i(0);
    if (!p.machine().config().headless || p.machine().config().input_script.empty())
        p.machine().clock().wait_periods(period < 1 ? 1 : period);
    ret_void(c);
}

void native_read_cpu_mhz(Cpu& c) { ret(c, 1000); }

void native_get_cpu_mhz(Cpu& c) { ret_fp(c, 1000.0); }

// get_gun_calibrated_x_y(int* x, int* y, uint* packet, int gun): the
// MegaJamma device (devices.cpp) puts each gun's shot in game coordinates in
// packet word 4 + gun (x low 16 bits, y high, signed; -1, -1 off screen).
void native_get_gun_calibrated_x_y(Cpu& c) {
    Args a{c};
    const std::uint32_t packet = a.u(2);
    const std::uint32_t gun = a.u(3) & 1;
    const std::uint32_t w = ld32(c.mem, packet + 4 * (4 + gun));
    const std::int32_t x = std::int16_t(w & 0xFFFF), y = std::int16_t(w >> 16);
    st32(c.mem, a.u(0), std::uint32_t(x));
    st32(c.mem, a.u(1), std::uint32_t(y));
    ret_void(c);
}

}

namespace voy::recomp {

void native_coin_free_play(Cpu& c) {
    Process& p = proc(c);
    if (p.machine().config().free_play) return ret(c, 1);
    // As the game: get_static_settings()->coins_per_credit <= 0.
    const std::uint32_t settings = call_guest(c, p.sym("get_static_settings"), {});
    ret(c, std::int32_t(ld32(c.mem, settings)) <= 0 ? 1u : 0u);
}

}

#pragma once
// Native replacements for guest functions (tools/x86tr/translate.py
// OVERRIDES_BY_NAME). Each is a cdecl callee: arguments are dwords at
// [esp+4], [esp+8], ..., the result is eax, and esp moves past the return
// address only. Defined in host/natives.cpp.
#include "recomp/cpu.hpp"

namespace voy::recomp {

void native_verify_ib(Cpu& c);          // int verify_ib(void): 1, an authenticated cabinet
void native_get_frame_number(Cpu& c);   // int get_frame_number(void): host 60 Hz vblank count
void native_wait_for_vblank(Cpu& c);    // void wait_for_vblank(int min_period)
void native_read_cpu_mhz(Cpu& c);       // int read_cpu_MHz(void): 1000
void native_get_cpu_mhz(Cpu& c);        // float get_cpu_mhz(void): 1000.0 in st(0)
void native_get_gun_calibrated_x_y(Cpu& c);  // (int* x, int* y, uint* packet, int gun)
void native_coin_free_play(Cpu& c);     // int coinFreePlay(void)

}

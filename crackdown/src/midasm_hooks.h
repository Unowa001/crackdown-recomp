// crackdown - the mid-asm hooks in config/midasm_hooks.toml
//
// The generated code declares each [[midasm_hook]] itself (extern, by name);
// this list is for the C++ side that defines them.

#pragma once

#include <rex/ppc/context.h>

double CrackdownMeasuredStepSeconds();
void CrackdownAudioTimeoutTick(PPCRegister& elapsed, PPCRegister& instance);

// hooks_framerate.cpp: replaces the game's fixed 33 ms per frame with the
// measured frame ms when --sim_game_clock is on. One hook per site (bit index,
// guest address of the `li rX,33`) so --sim_game_clock_sites can bisect them.
#define CRACKDOWN_FRAME_MS_SITES(X)                                          \
  X(0, 82287170) X(1, 82388E74) X(2, 823F0EB4) X(3, 8248F254)                \
  X(4, 824F9CF8) X(5, 824FB3E4) X(6, 82505ED0) X(7, 8257AAE8)                \
  X(8, 8259A3A8) X(9, 825E120C) X(10, 82651768) X(11, 826517CC)              \
  X(12, 826A78F4)

#define CRACKDOWN_DECLARE_FRAME_MS(bit, addr) void CrackdownFrameMs_##addr(PPCRegister& r);
CRACKDOWN_FRAME_MS_SITES(CRACKDOWN_DECLARE_FRAME_MS)
#undef CRACKDOWN_DECLARE_FRAME_MS

// hooks_storage.cpp: co-op menu checks of the HDD-cache flag (the `lbz` that
// loads it); they see "HDD present" while co-op networking is on.
#define CRACKDOWN_COOP_HDD_SITES(X) X(8265ADE4) X(82661C54) X(82678104)

#define CRACKDOWN_DECLARE_COOP_HDD(addr) void CrackdownCoopHdd_##addr(PPCRegister& r);
CRACKDOWN_COOP_HDD_SITES(CRACKDOWN_DECLARE_COOP_HDD)
#undef CRACKDOWN_DECLARE_COOP_HDD

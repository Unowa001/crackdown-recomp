// crackdown - storage hooks
//
// Guest 0x827457E8 is the game's statically linked XMountUtilityDrive wrapper
// (calls sub_82744E70 for an ID, then the real mount in sub_82745458, which
// opens the raw cache partition, checks the "Josh" header and creates the
// cache: device via StfsCreateDevice). Its only caller, sub_82322F28, sets the
// "utility-drive cache available" flag at 0x82DE3F9A when it returns 0.
//
// The SDK backs the cache partition with a NullDevice and stubs
// StfsCreateDevice/StfsControlDevice to SUCCESS but never provides
// \Device\cache0, so the mount "succeeds" while cache: stays unusable. The
// game then tries to copy fullcity_0100_EnvironmentBlocks.resblockc into
// cache: at boot (fails), and on New Game reads blocks through a cache:\...c
// handle that never opened: NtReadFile(-1) -> STATUS_INVALID_HANDLE, retried
// forever on the loading screen.
//
// Report "no hard drive" instead, like a 360 without an HDD: the flag stays
// clear and every loader streams straight from game:\. The caller only checks
// for nonzero; the exact code a drive-less console returns is unverified.

//
// Co-op Campaign needs a hard drive ("You need a hard drive to play
// co-operative Campaign mode", GameStrings.str 0x8AC6): sub_8265A738 enables
// "Play Now - Co-op" only if the profile is signed in (sub_82418938 = 1/2) and
// the flag at 0x82DE3F9A is set. Setting the flag itself sends New Game loads
// to cache:\fullcityc (hang, out/coop-live 2026-10-13), so with co-op
// networking on, only the co-op menu checks (config/midasm_hooks.toml) see it
// set; loaders keep streaming from game:\.

#include "generated/default/crackdown_init.h"
#include "midasm_hooks.h"

#include <rex/hook.h>
#include <rex/system/kernel_state.h>

namespace {
constexpr uint32_t kErrorDeviceNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED
}  // namespace

REX_HOOK_RAW(sub_827457E8) {
  (void)base;
  REXKRNL_INFO("XMountUtilityDrive (0x827457E8) -> no utility drive; streaming from game:");
  ctx.r3.u64 = kErrorDeviceNotConnected;
}

#define CRACKDOWN_DEFINE_COOP_HDD(addr)                        \
  void CrackdownCoopHdd_##addr(PPCRegister& r) {               \
    if (REX_KERNEL_STATE()->coop_network().enabled) r.u64 = 1; \
  }
CRACKDOWN_COOP_HDD_SITES(CRACKDOWN_DEFINE_COOP_HDD)
#undef CRACKDOWN_DEFINE_COOP_HDD

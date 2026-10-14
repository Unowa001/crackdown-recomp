// crackdown - event-object hooks
//
// Guest 0x82A4FA70 is a 3-instruction body:
//     li  r11,0
//     stw r11,4(r3)      ; KEVENT.Header.SignalState = 0
//     blr
// The job dispatcher sub_8225C538 calls it as the game's inlined ResetEvent
// (bl at 0x8225C614/6B4/6C0/748). It never reaches the kernel. The runtime
// waits on a host XEvent shadowing each guest KEVENT (sdk/src/system/xobject.cpp
// GetNativeObject, which assumes "PPC code [doesn't poke] the struct"), so the
// host event stays signaled. The dispatcher polls its 17 events with
// KeWaitForMultipleObjects (via sub_82A4FA80, timeout 0) and only starts the
// next queued job (sub_8225BF00) on WAIT_TIMEOUT (258). With a stuck-signaled
// event it never times out, so the job never starts and New Game hangs on
// the loading screen. Xenia doesn't hit this: its GetNativeObject calls
// XEvent::SyncFromGuest (xenia-canary 6e2caa5c, kernel/xobject.cc:434,
// kernel/xevent.cc:115), which the SDK dropped.
//
// The same body is also a C++ method (identical-code folding): its address
// sits in vtables at 0x8203503C and 0x8203505C (installed by sub_8290CB10,
// called through slot 0 by sub_8290B330), and in slot 3 of the vtable at 0x8207C68C. There r3 is an
// ordinary object, not a KEVENT. So only touch the host side when r3 is a
// dispatcher object the runtime already shadows; otherwise the guest store
// is the whole effect (a shadow created later reads SignalState from guest
// memory anyway).

#include "generated/default/crackdown_init.h"

#include <rex/hook.h>

namespace {
constexpr uint32_t kXObjSignature = 0x52455800;  // "REX\0" at +8, host handle at +12
}  // namespace

REX_HOOK_RAW(sub_82A4FA70) {
  // Original guest-visible effects: r11 = 0, [r3+4] = 0, r3 unchanged.
  ctx.r11.s64 = 0;
  REX_STORE_U32(ctx.r3.u32 + 4, 0);

  // Type byte 0/1 = notification/synchronization event.
  if (REX_LOAD_U8(ctx.r3.u32 + 0) <= 1 && REX_LOAD_U32(ctx.r3.u32 + 8) == kXObjSignature) {
    const uint64_t event_ptr = ctx.r3.u64;
    __imp__KeResetEvent(ctx, base);
    ctx.r3.u64 = event_ptr;
    ctx.r11.s64 = 0;
  }
}

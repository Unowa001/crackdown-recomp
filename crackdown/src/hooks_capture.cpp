// crackdown - diagnostic: F11 RenderDoc capture of whole guest frames
//
// RenderDoc's own capture key cuts frames at host Present, but the presenter
// runs on its own cadence while the GPU command thread records the guest's
// D3D12 work, so a host-frame capture holds only the final blit. Instead,
// when the app was launched from RenderDoc, F11 starts a capture at a guest
// swap and ends it two guest swaps later, so the capture holds at least one
// whole guest frame of draws.
//
// Guest 0x826BCA98 is the game's present routine, the only caller of the
// VdSwap import.

#include "generated/default/crackdown_init.h"

#include <rex/hook.h>
#include <rex/logging/macros.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <renderdoc/renderdoc_app.h>
#endif

namespace {
#if defined(_WIN32)
constexpr int kSwapsPerCapture = 2;

RENDERDOC_API_1_0_0* RenderDoc() {
  static RENDERDOC_API_1_0_0* api = [] {
    RENDERDOC_API_1_0_0* p = nullptr;
    // Only present when the process was launched or injected by RenderDoc.
    if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
      auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
      if (!get_api || !get_api(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&p))) {
        p = nullptr;
      }
    }
    if (p) {
      REXLOG_INFO("capture: RenderDoc attached; press F11 for a {}-guest-frame capture",
                  kSwapsPerCapture);
    }
    return p;
  }();
  return api;
}

bool g_f11_was_down = false;
int g_swaps_left = 0;  // > 0 while a capture is open

void OnGuestSwap() {
  RENDERDOC_API_1_0_0* api = RenderDoc();
  if (!api) {
    return;
  }
  if (g_swaps_left > 0) {
    if (--g_swaps_left == 0) {
      const uint32_t ok = api->EndFrameCapture(nullptr, nullptr);
      REXLOG_INFO("capture: ended ({})", ok ? "saved" : "FAILED");
    }
    return;
  }
  const bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
  if (down && !g_f11_was_down) {
    api->StartFrameCapture(nullptr, nullptr);
    g_swaps_left = kSwapsPerCapture;
    REXLOG_INFO("capture: started");
  }
  g_f11_was_down = down;
}
#else
void OnGuestSwap() {}
#endif
}  // namespace

REX_HOOK_RAW(sub_826BCA98) {
  OnGuestSwap();
  __imp__sub_826BCA98(ctx, base);
}

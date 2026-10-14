// crackdown - skip the boot logo videos (--skip_intro_videos, on by default)
//
// Guest 0x82742B28 is the game's statically linked CreateFileA: r3 = path,
// r4 = access, r5 = share mode, r7 = creation disposition (1..5), r8 = flags.
// On a bad disposition it calls sub_82746010 (NTSTATUS -> SetLastError) with
// 0xC000000D and returns -1 (INVALID_HANDLE_VALUE); after RtlInitAnsiString it
// goes on to NtCreateFile.
//
// The boot logos are Bink files in game:\XUIFrontEnd\ (MSGS.bik = Microsoft
// Game Studios, RTW_Logo.bik = Realtime Worlds). Failing their open the way the
// real CreateFileA fails a missing file (STATUS_OBJECT_NAME_NOT_FOUND, -1)
// lets the title skip them. Every .bik open is logged, so the boot order of
// the other short videos (CrackdownHD.bik, RiotActHD.bik) can be checked
// before adding them.

#include "generated/default/crackdown_init.h"
#include "xui_patch.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>

REXCVAR_DEFINE_BOOL(skip_intro_videos, true, "Crackdown",
                    "Skip the boot logo videos (MSGS.bik, RTW_Logo.bik)");

namespace {
constexpr uint32_t kStatusObjectNameNotFound = 0xC0000034;
constexpr std::string_view kSkipped[] = {"msgs.bik", "rtw_logo.bik"};

std::string GuestString(uint8_t* base, uint32_t addr, size_t max_len = 260) {
  std::string s;
  for (size_t i = 0; i < max_len; ++i) {
    const char c = static_cast<char>(REX_LOAD_U8(addr + static_cast<uint32_t>(i)));
    if (!c) {
      break;
    }
    s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return s;
}

bool EndsWith(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}
}  // namespace

// 0x82742B28 (generated .52.cpp): "mr r29,r3", "mr r4,r29",
// "addi r3,r1,104", "bl 0x82b673c4" (RtlInitAnsiString); the resulting
// ANSI_STRING is passed through OBJECT_ATTRIBUTES at +120 to
// "bl 0x82b673b4" (NtCreateFile) at 0x82742C78. Failure runs
// "bl 0x82746010" (SetLastError) then returns "li r3,-1".
// Standalone menu is read whole by 0x8237A240 -> 0x82743DF0/0x82742D18,
// mounted as memory:// by 0x82621990. Replace only r3 with an immutable
// guest-system-heap string; never overwrite the game's path buffer. Keep the
// existing intro skip logic below identical.
// Scene path: vfs index 0x8261F740 calls package open 0x826E7520;
// 0x826FF708 (.50.cpp:46262) reads its header/index; global vfs resolution
// 0x8261FC80 (.43.cpp) falls back from localized to global Scene.xzp.
// File stream 0x82700948 (.50.cpp:49007): at 0x82700A60 "lhz r9,0(r11)",
// "stb r9,0(r10)" converts the filename to ANSI; at 0x82700A94
// "mr r3,r29", "bl 0x82742b28" opens it through this same CreateFileA.
// Scene redirection is unverified at runtime (milestone 2). The standalone
// menu redirect was confirmed on the PC 2026-10-12 (crackdown_002).
REX_HOOK_RAW(sub_82742B28) {
  if (ctx.r3.u32) {
    const std::string path = GuestString(base, ctx.r3.u32);
    std::string canonical = path;
    std::replace(canonical.begin(), canonical.end(), '/', '\\');
    const bool menu = canonical == "game:\\xuifrontend\\menuhud_3.xzp";
    const bool scene = canonical == "game:\\xuifrontend\\scene.xzp";
    if (menu || scene) {
      const uint32_t replacement = menu ? crackdown::PCMenuGuestPath() : crackdown::PCSceneGuestPath();
      if (replacement) {
        const PPCContext original = ctx;
        ctx.r3.u64 = replacement;
        __imp__sub_82742B28(ctx, base);
        if (ctx.r3.u32 == 0xFFFFFFFF) {
          // Retry with all original arguments and disable both redirects/actions.
          crackdown::DisablePCMenu("patched package open failed");
          ctx = original;
          __imp__sub_82742B28(ctx, base);
        } else {
          static std::atomic<bool> logged_menu{false}, logged_scene{false};
          auto& logged = menu ? logged_menu : logged_scene;
          if (!logged.exchange(true))
            REXLOG_INFO("pc menu: redirect {} -> pcmod: (verified {})", path, menu ? "menuHUD_3" : "Scene");
        }
        return;
      }
    }
    if (EndsWith(path, ".bik")) {
      const std::string_view file = std::string_view(path).substr(path.find_last_of("\\/") + 1);
      const bool skip = REXCVAR_GET(skip_intro_videos) &&
                        std::find(std::begin(kSkipped), std::end(kSkipped), file) != std::end(kSkipped);
      REXLOG_INFO("intro: open {}{}", path, skip ? " -> skipped" : "");
      if (skip) {
        ctx.r3.u64 = kStatusObjectNameNotFound;
        sub_82746010(ctx, base);  // SetLastError(RtlNtStatusToDosError(status))
        ctx.r3.s64 = -1;          // INVALID_HANDLE_VALUE
        return;
      }
    }
  }
  __imp__sub_82742B28(ctx, base);
}

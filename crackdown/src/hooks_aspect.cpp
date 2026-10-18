// crackdown - other aspect ratios (--aspect_ratio, default 16:9)
//
// Guest 0x82BAA498 is the camera's aspect ratio (PC check 2026-10-17, docs/re-notes.md):
// its image value is 4:3 and the camera constructor sub_822846A0 overwrites it with 16:9
// (the float at 0x820ED554). The projection builders sub_82285058, sub_823265E0,
// sub_82295D10 and sub_82296748 read it; the only other writer is the leftover developer
// console command "widescreen" (sub_8250AC58, 4:3 <-> 16:9), which normal play never runs.
// Lead from saulob's README, verified against our build; no code taken from it.
//
// So after the constructor runs, the chosen ratio replaces the 16:9. The guest frame buffer
// gets the same shape from ApplyGameDefaults (host_tuning.cpp), which sets the video mode to
// 720 lines times the ratio unless --video_mode_width/height were set.

#include "generated/default/crackdown_init.h"

#include "host_tuning.h"

#include <bit>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging/macros.h>

REXCVAR_DEFINE_STRING(aspect_ratio, "16:9", "Crackdown",
                      "Game aspect ratio, as W:H (16:9, 16:10, 21:9, 32:9, 4:3) or a number "
                      "such as 2.37. Hor+: wider ratios show more to the sides");

namespace {
constexpr uint32_t kAspectAddress = 0x82BAA498;  // float, written by sub_822846A0
}

namespace crackdown {
double AspectRatio() {
  const std::string& text = REXCVAR_GET(aspect_ratio);
  double w = 0, h = 0;
  double ratio = 0;
  if (std::sscanf(text.c_str(), "%lf:%lf", &w, &h) == 2 && h > 0) {
    ratio = w / h;
  } else if (std::sscanf(text.c_str(), "%lf", &w) == 1) {
    ratio = w;
  }
  if (ratio < 1.0 || ratio > 4.0) {
    if (text != "16:9") {
      REXLOG_WARN("aspect_ratio '{}' not understood (use W:H between 1 and 4); using 16:9", text);
    }
    ratio = 16.0 / 9.0;
  }
  return ratio;
}

bool AspectRatioIsDefault() {
  const double ratio = AspectRatio();
  return ratio > 16.0 / 9.0 - 0.001 && ratio < 16.0 / 9.0 + 0.001;
}
}  // namespace crackdown

REX_HOOK_RAW(sub_822846A0) {
  __imp__sub_822846A0(ctx, base);
  if (!crackdown::AspectRatioIsDefault()) {
    REX_STORE_U32(kAspectAddress, std::bit_cast<uint32_t>(float(crackdown::AspectRatio())));
  }
}

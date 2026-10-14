// crackdown - experimental variable simulation step (--sim_variable_step,
// --sim_game_clock)
//
// Guest 0x826A7698 runs one frame of the game loop (disassembly): it calls the
// update sub_826A6B50 with dt = [0x82CFE4B4] * [0x820ED704], where 0x820ED704
// is the float 1/30 (the only copy in the image), then renders, runs the frame
// limiter (Sleep((1000 / [0x82BAA330]) - elapsed)) and returns. The step query
// sub_8254C568 multiplies by the same constant in 30 Hz mode, and about a
// dozen other sites use it to turn frame counts into seconds.
//
// So the step is a fixed amount per frame. PC tests (2026-10-11): uncapped at
// 60 fps the game ran at 2x speed; with the constant set to 1/60 (--sim_hz=60)
// it ran at the right speed while it held 60 fps, and slowed down in busy
// scenes where the frame rate fell to 35..50 (sim60b.csv).
//
// --sim_variable_step writes the measured time since the previous frame into
// the constant before each frame (clamped to 1/240..1/20 s, the clamp keeping
// hitches and loading stalls from becoming huge steps), so speed no longer
// follows frame rate. ApplySimRate (host_tuning.cpp) leaves the constant's
// page writable.
//
// That alone isn't enough (PC run crackdown_039, 2026-10-11 21:32): at 60 fps
// speech lines cut off early, the car elevator in the Agency base moves fast,
// and driving feels fast. The game has a second clock (disassembly): the
// object at [0x82DE25C0] (constructor sub_823225C0) has a "locked" byte at
// +13 that the constructor always sets to 1. Thirteen inlined sites (listed
// in config/midasm_hooks.toml) then add a fixed per-frame ms count to their
// timers:
//   ms = locked ? 33 : (byte [0x82DE3FB0] ? u32 [0x82D99228] : 16)
//
// Unlocking the clock to use the game's own variable path (a first version of
// --sim_game_clock, PC run crackdown_018) broke physics: the player was thrown
// into a wall and cars flew around, with nothing else rewriting the clock
// during the frame. The unlocked mode changes more than the ms count (the
// step query sub_8254C568, the frame counter at +16, the limiter).
//
// So --sim_game_clock keeps the clock locked and replaces only the literal:
// a mid-asm hook after each `li rX,33` puts the measured frame time, in whole
// ms, in the register. The rounding remainder carries to the next frame, and
// the step constant gets the same value (ms / 1000), so both clocks agree.

#include "generated/default/crackdown_init.h"

#include "ap/bridge.h"
#include "midasm_hooks.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>

REXCVAR_DEFINE_BOOL(sim_variable_step, false, "Crackdown",
                    "Experimental: rewrite the step constant every frame with the measured "
                    "frame time (clamped to 1/240..1/20 s), so game speed doesn't follow "
                    "frame rate");
REXCVAR_DEFINE_BOOL(sim_game_clock, false, "Crackdown",
                    "Like --sim_variable_step, and also feed the measured frame time to the "
                    "game's fixed 33 ms per-frame timers (speech, timed objects). On by "
                    "default (host_tuning.cpp ApplyGameDefaults)");
REXCVAR_DEFINE_BOOL(sim_step_literal, false, "Crackdown",
                    "With --sim_game_clock/--sim_variable_step: leave the shared 1/30 literal "
                    "(0x820ED704) alone and scale only the main-loop update dt and the step "
                    "query. On by default");
REXCVAR_DEFINE_BOOL(sim_frame_counter_30hz, false, "Crackdown",
                    "Experimental, with --sim_game_clock: advance the game clock's frame "
                    "counter ([0x82DE25C0]+16) at 30 Hz instead of once per frame");
REXCVAR_DEFINE_BOOL(sim_anim_step, false, "Crackdown",
                    "With --sim_game_clock: also write the measured step into the animation "
                    "clock's own 1/30 (0x820ED7CC, sub_82330888) and the 0.033 s timer step "
                    "(0x82CFF2B4, sub_823FF3B8). On by default");
REXCVAR_DEFINE_BOOL(sim_ground_snap_30hz, true, "Crackdown",
                    "Above 30 fps, give the player's step-down snap (sub_822B4D40) the "
                    "velocity it has at 30 fps instead of drop / frame time (stairs speed bug)");
REXCVAR_DECLARE(bool, diag_char_proxy);
REXCVAR_DEFINE_STRING(frame_log, "", "Crackdown",
                      "Benchmark: write each guest frame's host time (frame_time_us, one row per "
                      "frame, tools/frametimes.py reads it) to this CSV; works in Release builds");
REXCVAR_DEFINE_INT32(sim_game_clock_sites, 0x1FFF, "Crackdown",
                     "Experimental: bitmask of the 33 ms sites --sim_game_clock rewrites "
                     "(bit order as in config/midasm_hooks.toml); for bisecting");

namespace {
constexpr uint32_t kStepConstantAddress = 0x820ED704;
constexpr uint32_t kLockedFrameMs = 33;
constexpr double kMinStep = 1.0 / 240.0;
constexpr double kMaxStep = 1.0 / 20.0;

using Clock = std::chrono::steady_clock;
Clock::time_point g_last_frame{};
double g_ms_carry = 0.0;
// Read by the mid-asm hooks, which may run on other threads.
std::atomic<uint32_t> g_frame_ms{kLockedFrameMs};
// --sim_step_literal: measured step / (1/30), applied to the update dt and the
// step query instead of rewriting the literal.
std::atomic<float> g_step_scale{1.0f};
// This frame's step in seconds (1/30 until the first measured frame).
std::atomic<float> g_step{1.0f / 30.0f};
double g_counter_carry = 0.0;
constexpr uint32_t kClockPointerAddress = 0x82DE25C0;
constexpr uint32_t kAnimStepAddress = 0x820ED7CC;   // 1/30, read only by sub_82330888
constexpr uint32_t kTimerStepAddress = 0x82CFF2B4;  // 0.033, read only by sub_823FF3B8

// The flags are read once, at the first hook call (the guest starts after
// ApplyGameDefaults has set them). The hooks rewrite guest literals and keep
// clock state, which switching a flag off mid-game would not undo (Codex
// review 2026-10-12), so they only take effect at startup.
struct SimFlags {
  bool variable_step = REXCVAR_GET(sim_variable_step);
  bool game_clock = REXCVAR_GET(sim_game_clock);
  bool step_literal = REXCVAR_GET(sim_step_literal);
  bool anim_step = REXCVAR_GET(sim_anim_step);
  bool frame_counter_30hz = REXCVAR_GET(sim_frame_counter_30hz);
  bool ground_snap_30hz = REXCVAR_GET(sim_ground_snap_30hz);
  int32_t game_clock_sites = REXCVAR_GET(sim_game_clock_sites);
};
const SimFlags& Sim() {
  static const SimFlags flags;
  return flags;
}

// The ground-snap fix applies only to frames clearly shorter than 1/30 s: a
// measured 30 fps frame is 33 or 34 ms, which must stay stock.
constexpr float kGroundSnapMaxDt = 1.0f / 32.0f;
}  // namespace

// Consumers of 30 Hz counters use the same clamped simulation
// step as the game update. Stock clocks must advance exactly one tick.
double CrackdownMeasuredStepSeconds() {
  if (!Sim().variable_step && !Sim().game_clock) return 1.0 / 30.0;
  return double(g_step.load(std::memory_order_relaxed));
}

// Leaves the 33 in place unless the site's bit is set. g_frame_ms stays 33
// while --sim_game_clock is off.
#define CRACKDOWN_DEFINE_FRAME_MS(bit, addr)                  \
  void CrackdownFrameMs_##addr(PPCRegister& r) {              \
    if (Sim().game_clock_sites & (1 << (bit))) {             \
      r.u64 = g_frame_ms.load(std::memory_order_relaxed);     \
    }                                                         \
  }
CRACKDOWN_FRAME_MS_SITES(CRACKDOWN_DEFINE_FRAME_MS)
#undef CRACKDOWN_DEFINE_FRAME_MS

// --frame_log: written only from this hook (the guest's once-per-frame timer
// update). The window-close path fflushes all streams (FlushPerfCsvLog); a
// row cut short there is dropped by tools/frametimes.py.
static void LogFrameTime() {
  static std::FILE* f = [] {
    const std::string& path = REXCVAR_GET(frame_log);
    std::FILE* file = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
    if (file) {
      std::setvbuf(file, nullptr, _IOFBF, 1 << 16);
      std::fputs("frame_time_us\n", file);
    }
    return file;
  }();
  static Clock::time_point last{};
  if (!f) return;
  const Clock::time_point now = Clock::now();
  if (last != Clock::time_point{}) {
    std::fprintf(f, "%lld\n", static_cast<long long>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - last).count()));
  }
  last = now;
}

REX_HOOK_RAW(sub_826A7698) {
  LogFrameTime();
  const bool game_clock = Sim().game_clock;
  if (Sim().variable_step || game_clock) {
    const Clock::time_point now = Clock::now();
    if (g_last_frame != Clock::time_point{}) {
      const double dt = std::chrono::duration<double>(now - g_last_frame).count();
      float step = float(std::clamp(dt, kMinStep, kMaxStep));
      if (game_clock) {
        g_ms_carry += double(step) * 1000.0;
        const uint32_t ms = uint32_t(std::floor(g_ms_carry));  // >= 4: step >= 1/240 s
        g_ms_carry -= ms;
        g_frame_ms.store(ms, std::memory_order_relaxed);
        step = float(ms) / 1000.0f;
      }
      g_step.store(step, std::memory_order_relaxed);
      if (Sim().anim_step) {
        REX_STORE_U32(kAnimStepAddress, std::bit_cast<uint32_t>(step));
        REX_STORE_U32(kTimerStepAddress, std::bit_cast<uint32_t>(step));
      }
      if (Sim().step_literal) {
        g_step_scale.store(step * 30.0f, std::memory_order_relaxed);
      } else {
        REX_STORE_U32(kStepConstantAddress, std::bit_cast<uint32_t>(step));
      }
    }
    g_last_frame = now;
  }
  __imp__sub_826A7698(ctx, base);
  crackdown::ap::OnFrame(ctx, base);  // AP bridge: queued network work + grants
}

// The constant is the image's only 1/30, so it is also Havok's default
// hkWorldCinfo::m_expectedMinPsiDeltaTime: the hkWorldCinfo constructor
// sub_8298D618 does `lfs f9,[0x820ED704]; stfs f9,92(r3)`, and Havok's own
// reflection table for hkWorldCinfo puts expectedMinPsiDeltaTime at +92.
// Havok sizes its collision tolerances from it when a world is built, so
// with the constant rewritten every frame the world got whatever frame time
// was current at load. PC run crackdown_031 (2026-10-12, --fps_cap=60
// --sim_game_clock): physics got the right step (--diag_physics_dt), but
// running down stairs still built up huge speed and cars hitting the bottom
// of the stairs were wrecked instantly. Keep Havok's 1/30 there.
REX_HOOK_RAW(sub_8298D618) {
  const uint32_t cinfo = ctx.r3.u32;
  __imp__sub_8298D618(ctx, base);
  if (Sim().variable_step || Sim().game_clock) {
    constexpr uint32_t kOneThirtieth = 0x3D088889;  // 1/30, the image's literal
    const uint32_t got = REX_LOAD_U32(cinfo + 92);
    if (got != kOneThirtieth) {
      REXLOG_INFO("sim_game_clock: hkWorldCinfo {:08X} expectedMinPsiDeltaTime {} -> 1/30",
                  cinfo, std::bit_cast<float>(got));
      REX_STORE_U32(cinfo + 92, kOneThirtieth);
    }
  }
}

// --sim_step_literal (bisecting the 60 fps stairs bug): the literal 0x820ED704
// has a dozen direct readers besides the game step (see docs/60fps-audit.md).
// Leave it at 1/30 and scale only the two step sources:
// - the main loop's update, sub_826A6B50(f1 = [0x82CFE4B4] * [0x820ED704]),
//   called only from sub_826A7698;
// - the step query sub_8254C568, which with the clock locked returns
//   [0x82C63578] * [0x820ED704] (the unlocked paths aren't used).
REX_HOOK_RAW(sub_826A6B50) {
  if (Sim().step_literal) {
    ctx.f1.f64 = double(float(ctx.f1.f64) * g_step_scale.load(std::memory_order_relaxed));
  }
  __imp__sub_826A6B50(ctx, base);
}

REX_HOOK_RAW(sub_8254C568) {
  __imp__sub_8254C568(ctx, base);
  if (Sim().step_literal) {
    ctx.f1.f64 = double(float(ctx.f1.f64) * g_step_scale.load(std::memory_order_relaxed));
  }
}

// --sim_frame_counter_30hz (bisecting the 60 fps stairs bug): sub_82324938,
// called once per frame from the main loop, does `[clock+16] += 1` while the
// clock is locked (+13) and not paused (+14). About a dozen functions use that
// counter for frame deadlines (docs/60fps-audit.md), which expire twice as
// fast at 60 fps. Re-advance it by the number of whole 30 Hz ticks elapsed.
REX_HOOK_RAW(sub_82324938) {
  const uint32_t clock = REX_LOAD_U32(kClockPointerAddress);
  const uint32_t before = clock ? REX_LOAD_U32(clock + 16) : 0;
  __imp__sub_82324938(ctx, base);
  if (!clock || !Sim().frame_counter_30hz || !Sim().game_clock) {
    return;
  }
  if (REX_LOAD_U32(clock + 16) != before + 1) {
    return;  // paused, unlocked or reset this frame: leave it alone
  }
  g_counter_carry += double(g_step.load(std::memory_order_relaxed)) * 30.0;
  const uint32_t ticks = uint32_t(std::floor(g_counter_carry));
  g_counter_carry -= ticks;
  REX_STORE_U32(clock + 16, before + ticks);
}

// Stairs speed bug at 60 fps. The character controller update sub_822A9E78
// builds the velocity it hands Havok's character proxy at controller+352:
// move velocity, plus the correction of either sub_822B4100 (step up) or
// sub_822B4D40 (step down / stick to ground), plus gravity, then
// sub_822ACF68(dt) does setLinearVelocity and hkCharacterProxy::integrate
// (sub_822BF268). sub_822B4D40(controller r3, dt f1, out r5) ends with
// (0x822B562C):
//     out.y = -clamp(target - height, 0, max) * gain / dt
// i.e. it pulls the player down the whole drop in one step. A stair step is a
// fixed height, so at 60 fps that velocity doubles. The proxy's solver
// projects it onto the stair-edge plane, which turns part of it into forward
// speed, and the controller keeps the proxy's output velocity: --diag_char_proxy
// on the same replayed stairs run (bench/stairs.rec) shows input vy down to
// -7.6 m/s and a 13.8 m/s peak at 30 fps (crackdown_047), vy -19.2 and
// 28-32 m/s at 60 fps (crackdown_048/049); running speed is 11.2 m/s.
// Dividing by max(dt, 1/30) instead gives the snap its 30 fps velocity, so it
// covers the drop over the same time instead of in one 60 Hz frame. At 30 fps
// (measured 33/34 ms frames included, see kGroundSnapMaxDt) nothing changes.
REX_HOOK_RAW(sub_822B4D40) {
  const uint32_t out = ctx.r5.u32;
  const float dt = float(ctx.f1.f64);
  const uint32_t before = REX_LOAD_U32(out + 4);
  __imp__sub_822B4D40(ctx, base);
  const uint32_t after = REX_LOAD_U32(out + 4);
  if (after == before) {
    return;  // no snap this step
  }
  float y = std::bit_cast<float>(after);
  const float y_game = y;
  if (Sim().ground_snap_30hz && dt > 0.0f && dt < kGroundSnapMaxDt) {
    y *= dt * 30.0f;
    REX_STORE_U32(out + 4, std::bit_cast<uint32_t>(y));
  }
  if (REXCVAR_GET(diag_char_proxy) && std::fabs(y_game) > 0.5f) {
    REXLOG_INFO("diag_char_proxy: ground snap dt={:.4f} out.y={:.3f} -> {:.3f} ret={}", dt, y_game, y,
                ctx.r3.u32 & 0xFF);
  }
}

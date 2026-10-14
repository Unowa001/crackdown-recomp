// crackdown - host-side setup that the SDK leaves out
//
// perf CSV: the SDK defines --perf_log_csv and writes a row on every guest swap
// (Profiler::Flip), but nothing ever opens the file (SetCsvLogPath has no
// callers), so the flag silently did nothing.
//
// Tracy: SDK v0.10 (69f447e "perf: make tracy startup explicit") took
// Profiler::Startup() out of the runtime and left it to the app, and gates
// every zone on TracyIsStarted. Nobody called it, so profiling builds never
// opened Tracy's listener and tracy-capture waited forever (PC 2026-10-11:
// no TCP listen on 8086 while the game ran). Release builds compile it out.
//
// Game defaults: SDK settings Crackdown needs that the SDK leaves off.
// readback_resolve=fast: the game reads a GPU-resolved texture back on the CPU
// (apparently the scene brightness for exposure/bloom). With the SDK default
// "none" the CPU never sees it, and sunlit surfaces are blown out to near
// white while shadowed areas look right. "fast" hands over the previous
// frame's copy, which is enough for a value that adapts over time. Same fix
// as d3d12_readback_resolve in Xenia Canary's compatibility report for
// 4D5307DC ("fixes the blown-out bloom issue"). Checked on the PC
// 2026-10-11: --readback_resolve=fast makes the city look correct.
// readback_resolve_max_bytes=262144 (SDK patch 0008): the game resolves ~21
// targets a frame (~27 MB); copying all of them back cost the GPU command
// thread ~1 ms a frame. Only resolves up to 256 KB (128x128 and 320x180) are
// read back now. PC 2026-10-12, spawn screenshots 15 s into gameplay: mean
// luma 76.1 with all, 76.1 with the limit (mean |diff| 0.66), 163.3 with none
// (blown out). Release pan bench 84.2 -> 94.4 fps avg, 1% low 15.3 -> 13.8 ms.

#include "host_tuning.h"

#include <string>
#include <string_view>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging/macros.h>
#include <rex/memory/utils.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

// Defined in the SDK's perf/counter.cpp, which isn't built at all when the SDK
// is configured with REXGLUE_ENABLE_PERF_COUNTERS=OFF; its rows are only
// written where the define is set (not Release), so only reference it there.
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
REXCVAR_DECLARE(std::string, perf_log_csv);
#endif
REXCVAR_DECLARE(std::string, gpu_plugin);  // SDK rex_app.cpp
REXCVAR_DECLARE(bool, sim_variable_step);  // hooks_framerate.cpp
REXCVAR_DECLARE(bool, sim_game_clock);     // hooks_framerate.cpp
REXCVAR_DECLARE(bool, sim_step_literal);   // hooks_framerate.cpp

// Guest 0x82BAA330: s32 frame cap read by the render loop's limiter
// (sub_826A65F0 / sub_826A7698: Sleep((1000 / cap) - elapsed) when a frame
// finishes early). Static data, 32 in the shipped game; only debug commands
// (0x8232BDD8 / 0x8232BE20) write it. The guest's wait for two vblanks is
// what really holds 30 fps; --vsync=false lifts that, and this cap is then
// the limit.
REXCVAR_DEFINE_INT32(fps_cap, 0, "Crackdown",
                     "Write the game's frame cap (guest 0x82BAA330) at startup; 0 = leave it "
                     "(32). Needs --vsync=false to go past 30 fps. Unless set, the game "
                     "defaults make it 60 (with vsync off and the --sim_* clock flags on)")
    .range(0, 1000);

namespace crackdown {

void ApplyGameDefaults() {
  struct Default {
    const char* name;
    const char* value;
  };
  // True 60 Hz simulation (2026-10-12, docs/60fps-audit.md): vsync off lifts
  // the guest's two-vblank wait, the frame cap holds 60, the game clock and
  // the main-loop step follow the measured frame time (the shared 1/30
  // literal stays put), and the animation clock's own 1/30 and the 0.033 s
  // timer follow too. Verified by replay A/B against 30 fps: stairs (with
  // --sim_ground_snap_30hz), driving speed, impacts and car damage.
  // An explicit --vsync=true means stock 30 fps: then none of the 60 fps
  // set is applied (with it, frames are still timed by the host clock, so the
  // steps would be measured 33/34 ms instead of the game's exact 1/30; Codex
  // review 2026-10-12). An explicit --sim_hz or --sim_variable_step picks an
  // older fixed/variable step mode, which the measured clock must not mix
  // with (sim_hz would scale the update by 1/60 / 1/30 a second time).
  static constexpr Default kDefaults[] = {
      {"readback_resolve", "fast"},
      {"readback_resolve_max_bytes", "262144"},
      {"vsync", "false"},
      {"fps_cap", "60"},
      {"sim_game_clock", "true"},
      {"sim_step_literal", "true"},
      {"sim_anim_step", "true"},
      {"sim_audio_timeout_30hz", "true"},
  };
  auto user_set = [](const char* name) {
    return rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault;
  };
  // System Link co-op drops with "Connection to other player lost" about a
  // second after spawning under the measured clock, and holds at stock 30 fps
  // (two instances, out/coop-live 2026-10-13); the netcode's timers are not
  // audited for 60 Hz yet. So co-op networking means stock 30 fps unless
  // --vsync is set explicitly.
  const bool coop = rex::cvar::Query<std::string>("net_mode") != "off";
  const bool stock = user_set("vsync") ? rex::cvar::Query<bool>("vsync") : coop;
  const bool other_step_mode =
      (user_set("sim_hz") && rex::cvar::Query<int32_t>("sim_hz") > 0) ||
      (user_set("sim_variable_step") && REXCVAR_GET(sim_variable_step));
  if (stock && !user_set("vsync")) {
    REXLOG_INFO("crackdown defaults: co-op networking on, stock 30 fps (60 fps defaults not "
                "applied; --vsync=false overrides)");
  } else if (stock) {
    REXLOG_INFO("crackdown defaults: --vsync=true, stock 30 fps (60 fps defaults not applied)");
  } else if (other_step_mode) {
    REXLOG_INFO("crackdown defaults: --sim_hz/--sim_variable_step set, measured-clock "
                "defaults not applied");
  }
  for (const Default& d : kDefaults) {
    const std::string_view name = d.name;
    if (!name.starts_with("readback_resolve") && stock) {
      continue;
    }
    if (name.starts_with("sim_") && other_step_mode) {
      continue;
    }
    // Only where nothing else set it (config file, environment, command line).
    // GetFlagSource also reports kDefault for a flag that isn't registered.
    if (user_set(d.name)) {
      continue;
    }
    // Flags defined in the GPU plugin exist only once it has loaded, so this
    // runs from OnPreLaunchModule; a failure here means it ran too early.
    if (rex::cvar::SetFlagByName(d.name, d.value)) {
      REXLOG_INFO("crackdown default: --{}={}", d.name, d.value);
    } else {
      REXLOG_WARN("crackdown default: could not set --{}={}", d.name, d.value);
    }
  }
}

void ApplyGpuPluginDefault() {
  // Runs from OnPostInitLogging: after the config file has loaded (so a
  // gpu_plugin set there wins; SetFlagByName marks the value kRuntime, which
  // would outrank the config if it ran first) and before the plugin is chosen.
  if (rex::cvar::GetFlagSource("gpu_plugin") == rex::cvar::Source::kDefault &&
      REXCVAR_GET(gpu_plugin).empty()) {
    rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    REXLOG_INFO("crackdown default: --gpu_plugin=xenos");
  }
}

void ApplyStartupDefaults(rex::PathConfig& paths) {
  if (!paths.game_data_root.empty()) {
    return;
  }
  // The dev build sits in crackdown/out/build/<preset>/ under the repo, whose
  // game/ folder holds the dump; an installed copy has game/ next to the exe.
  std::filesystem::path dir = rex::filesystem::GetExecutableFolder();
  for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
    const std::filesystem::path candidate = dir / "game";
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate / "default.xex", ec)) {
      paths.game_data_root = candidate;
      REXLOG_INFO("crackdown default: --game_data_root={}", candidate.string());
      return;
    }
    if (!dir.has_parent_path() || dir.parent_path() == dir) {
      break;
    }
    dir = dir.parent_path();
  }
}

// Guest 0x820ED704: the float 1/30 (the only copy in the image). It is the
// simulation step: the main loop (sub_826A7698) passes [0x82CFE4B4] * it to
// the update (sub_826A6B50) every frame, and the step query sub_8254C568
// returns [0x82C63578] * it in 30 Hz mode. About a dozen other sites use it to
// turn frame counts into seconds. Uncapping the frame rate without changing
// it ran the game at 2x speed (PC test 2026-10-11). Clearing the 30 Hz flag
// instead ([[0x82DE25C0]+13], which selects 1/60 in sub_8254C568 only) mixed
// 1/30 and 1/60 steps and threw the player into the ocean.
REXCVAR_DEFINE_INT32(sim_hz, 0, "Crackdown",
                     "Experimental: simulation steps per second (writes 1/N over the game's "
                     "1/30 step constant at 0x820ED704); 0 = leave it (30). Pair with a "
                     "frame rate of N")
    .range(0, 240);

void ApplySimRate(rex::memory::Memory* memory) {
  constexpr uint32_t kStepConstantAddress = 0x820ED704;
  int32_t hz = REXCVAR_GET(sim_hz);
  const bool game_clock = REXCVAR_GET(sim_game_clock);
  if (hz > 0 && (game_clock || REXCVAR_GET(sim_variable_step))) {
    // Both set explicitly: the measured step already sets it. (sim_step_literal
    // alone is inert: its scale stays 1 without a measured-step mode.)
    REXLOG_WARN("sim_hz: ignored with --sim_game_clock/--sim_variable_step");
    hz = 0;
  }
  const bool variable = REXCVAR_GET(sim_variable_step) || game_clock;
  if ((hz <= 0 && !variable) || !memory) {
    return;
  }
  // The constant is in .rdata, mapped read-only. Leave the page writable:
  // the variable step rewrites it every frame, and nothing else relies on
  // that page faulting.
  auto* heap = memory->LookupHeap(kStepConstantAddress);
  if (!heap || !heap->Protect(kStepConstantAddress, 4,
                              rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_WARN("sim_hz: could not unprotect {:08X}", kStepConstantAddress);
    return;
  }
  if (hz > 0) {
    auto* p = memory->TranslateVirtual<uint8_t*>(kStepConstantAddress);
    const float old_step = rex::memory::load_and_swap<float>(p);
    const float step = 1.0f / float(hz);
    rex::memory::store_and_swap<float>(p, step);
    REXLOG_INFO("sim_hz: step constant {} -> {} ({} Hz)", old_step, step, hz);
  }
  if (game_clock) {
    REXLOG_INFO("sim_game_clock: step constant and game ms clock follow measured frame time");
  } else if (variable) {
    REXLOG_INFO("sim_variable_step: step constant follows measured frame time");
  }
}

void ApplyFpsCap(rex::memory::Memory* memory) {
  constexpr uint32_t kFrameCapAddress = 0x82BAA330;
  const int32_t cap = REXCVAR_GET(fps_cap);
  if (cap <= 0 || !memory) {
    return;
  }
  auto* p = memory->TranslateVirtual<uint8_t*>(kFrameCapAddress);
  const int32_t old_cap = rex::memory::load_and_swap<int32_t>(p);
  rex::memory::store_and_swap<int32_t>(p, cap);
  REXLOG_INFO("fps_cap: guest frame cap {} -> {}", old_cap, cap);
}

REXCVAR_DEFINE_STRING(dump_image, "", "Crackdown",
                      "Diagnostic: write the loaded guest image (base..base+size, as mapped "
                      "before the guest starts) to this file");

void DumpGuestImage(rex::Runtime* runtime) {
  const std::string& path = REXCVAR_GET(dump_image);
  if (path.empty() || !runtime || !runtime->kernel_state()) {
    return;
  }
  auto module = runtime->kernel_state()->GetExecutableModule();
  if (!module || !module->xex_module()) {
    REXLOG_WARN("dump_image: no executable module");
    return;
  }
  const uint32_t base = module->xex_module()->base_address();
  const uint32_t size = module->xex_module()->image_size();
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(runtime->memory()->TranslateVirtual<uint8_t*>(base)),
            size);
  REXLOG_INFO("dump_image: {:08X}..{:08X} ({} bytes) -> {}", base, base + size, size, path);
}

void OpenPerfCsvLog() {
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
  const std::string& path = REXCVAR_GET(perf_log_csv);
  if (path.empty()) {
    return;
  }
  rex::perf::SetCsvLogPath(path);
  REXLOG_INFO("perf: writing per-frame counters to {}", path);
#endif
}

void FlushPerfCsvLog() {
  // Also flushes --frame_log (hooks_framerate.cpp), in every build type.
  // The SDK only fflushes the CSV every 60 rows, and window close hard-exits
  // (std::_Exit) without its profiler shutdown. fflush(nullptr) instead of
  // rex::perf::FlushCsv(): that one fcloses the file while the swap path (the
  // GPU command thread, which keeps running until the exit) may still be
  // writing a row; the CRT locks each stream for fflush. The SDK writes a row
  // in several calls and has no lock to wait on, so the file can end with a
  // partial row: tools/frametimes.py drops it. Rows after this are lost,
  // which the benchmark's end trim covers.
  std::fflush(nullptr);
}

void StartProfiler() {
#ifdef REXGLUE_ENABLE_PROFILING
  rex::perf::Profiler::Startup();
  REXLOG_INFO("perf: Tracy client started (on demand, connect any time)");
#endif
}

}  // namespace crackdown

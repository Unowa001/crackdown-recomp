// crackdown - host-side setup that the SDK leaves out

#pragma once

namespace rex {
class Runtime;
struct PathConfig;
}
namespace rex::memory {
class Memory;
}

namespace crackdown {

// Open the --perf_log_csv file, if set. The SDK defines the flag but never opens it.
void OpenPerfCsvLog();

// Flush the --perf_log_csv rows written so far (window close hard-exits).
void FlushPerfCsvLog();

// Start Tracy's client in profiling builds. Since SDK v0.10 the app has to.
void StartProfiler();

// Set SDK flags Crackdown needs, unless the user set them.
void ApplyGameDefaults();

// Defaults for starting the exe with no arguments (double-click, launchers):
// --game_data_root = the first "game" folder holding default.xex next to the
// exe or in one of its parent folders.
void ApplyStartupDefaults(rex::PathConfig& paths);

// --gpu_plugin=xenos unless set anywhere (config file included).
void ApplyGpuPluginDefault();

// --sim_hz: write 1/N over the game's 1/30 step constant (0x820ED704).
void ApplySimRate(rex::memory::Memory* memory);

// --fps_cap: write the guest frame cap (0x82BAA330) before the guest starts.
void ApplyFpsCap(rex::memory::Memory* memory);

// --dump_image: write the loaded guest image to a file (diagnostic).
void DumpGuestImage(rex::Runtime* runtime);

}  // namespace crackdown

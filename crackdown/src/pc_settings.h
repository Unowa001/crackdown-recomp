// Pending selections: never mutate live SDK cvars from a guest thread.
#pragma once
#include <array>
#include <cstddef>
#include <filesystem>
#include <mutex>

namespace crackdown {
enum class PCSetting : size_t { framerate, resolution, display };
class PCSettingsStore {
 public:
  void Initialize(const std::filesystem::path& path, bool vsync, int scale, bool fullscreen);
  int Selection(PCSetting setting) const;
  // Saves exactly one key; defaults remove overrides. Returns false for no change.
  // Throws on failure; pending state advances only after a successful file edit.
  bool Save(PCSetting setting, int selection);
 private:
  mutable std::mutex mutex_;
  std::filesystem::path path_;
  std::array<int, 3> selections_{};
};
PCSettingsStore& PCSettings();
}

#include "pc_settings.h"
#include "../common/toml_edit.h"
#include <stdexcept>

namespace crackdown {
PCSettingsStore& PCSettings() {
  static PCSettingsStore settings;
  return settings;
}
void PCSettingsStore::Initialize(const std::filesystem::path& path, bool vsync, int scale, bool fullscreen) {
  std::lock_guard lock(mutex_);
  path_ = path;
  // Preserve unsupported scales until the owner deliberately selects a preset.
  selections_ = {vsync ? 1 : 0, scale == 1 ? 0 : scale == 2 ? 1 : -1, fullscreen ? 0 : 1};
}
int PCSettingsStore::Selection(PCSetting setting) const {
  std::lock_guard lock(mutex_);
  return selections_.at(static_cast<size_t>(setting));
}
bool PCSettingsStore::Save(PCSetting setting, int selection) {
  std::lock_guard lock(mutex_);
  if (selection != 0 && selection != 1) throw std::runtime_error("Invalid PC selection");
  const auto index = static_cast<size_t>(setting);
  if (index >= selections_.size() || path_.empty()) throw std::runtime_error("PC settings not initialized");
  if (selections_[index] == selection) return false;
  const char* keys[] = {"vsync", "resolution_scale", "fullscreen"};
  const char* overrides[] = {"true", "2", "false"};
  config::update_config(path_, {{keys[index], selection == 0 ? std::nullopt :
                                std::optional<std::string>(overrides[index])}});
  selections_[index] = selection;
  return true;
}
}

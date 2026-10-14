// Source-preserving config edits shared by the launcher and native PC page.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <toml++/toml.hpp>

namespace crackdown::config {
namespace fs = std::filesystem;
class ConfigTemp {
 public:
  explicit ConfigTemp(const fs::path& target);
  ~ConfigTemp();
  ConfigTemp(const ConfigTemp&) = delete;
  ConfigTemp& operator=(const ConfigTemp&) = delete;
  const fs::path& path() const { return path_; }
  void replace(const fs::path& target, const std::string& text);
 private:
  fs::path path_;
  void* handle_=nullptr;
  int fd_=-1;
};
toml::table read_config(const fs::path& path);
const toml::node* setting(const toml::table& table, const std::string& flat_key);
// Null removes an override; untouched source text and comments are retained.
using Changes = std::map<std::string, std::optional<std::string>>;
void update_config(const fs::path& path, const Changes& changes);
std::string toml_string(const std::string& text);
}

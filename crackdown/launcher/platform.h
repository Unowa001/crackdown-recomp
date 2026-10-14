#pragma once
#include "core.h"
#include <chrono>
#include <optional>
namespace launcher {
// One lock per canonical package directory. Held for the launcher's lifetime.
class InstanceLock {
 public:
  explicit InstanceLock(const fs::path& home);
  ~InstanceLock();
  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;
 private:
  void* handle_=nullptr;
  int fd_=-1;
};
struct ProcessOwner { uint64_t pid, started; };
ProcessOwner current_process_owner();
// Unknown/inaccessible owners are treated as alive, so cleanup is conservative.
bool owner_alive(ProcessOwner owner);
// Exclusively created sibling temporary file; destruction removes only this file.
using crackdown::config::ConfigTemp;
// POSIX callback is SDL_OpenURL in the UI; Windows opens the native wide path.
void open_folder(const fs::path& path, const std::function<bool(const char*)>& open_url = {});
std::string folder_url(const fs::path& path);
class GameProcess {
 public:
  ~GameProcess();
  // Arguments are UTF-8 "--name=value" flags, passed without a shell.
  void start(const fs::path& home, const std::vector<std::string>& args);
  std::optional<int> poll();
  bool running() const { return active_; }
  double elapsed() const;
 private:
  bool active_=false;
  void* handle_=nullptr;
  int pid_=0;
  std::chrono::steady_clock::time_point started_;
};
std::string system_info();
// The SDK's default user data folder: Documents\crackdown on Windows (rex::filesystem::GetUserFolder()).
fs::path default_user_folder();
std::string newest_log_tail(const fs::path& home, fs::file_time_type since);
}

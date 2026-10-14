#include "xui_patch.h"

#include "sha256.h"  // sdk/thirdparty/crypto (SDK 0.10 no longer installs rex/crypto/sha256.h)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging/macros.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_BOOL(pc_menu_mod, true, "Crackdown",
                    "Add PC Settings and Quit to Desktop to the game's menu (restart required)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace crackdown {
namespace {
namespace fs = std::filesystem;
constexpr size_t kHeader = 92;
constexpr uint64_t kMaxPackage = 64 * 1024 * 1024;
constexpr uint64_t kMaxPatch = 8 * 1024 * 1024;
constexpr std::string_view kDevice = "\\Device\\CrackdownPCMod";
std::atomic<uint32_t> guest_path{0};
std::atomic<uint32_t> scene_path{0};
thread_local std::chrono::steady_clock::duration hash_time{};
std::atomic<bool> warned{false};

[[noreturn]] void Fail(const char* reason) { throw std::runtime_error(reason); }

uint64_t ReadBE(std::span<const uint8_t> bytes, size_t& pos, size_t count) {
  if (pos > bytes.size() || count > bytes.size() - pos) Fail("truncated patch field");
  uint64_t value = 0;
  for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[pos++];
  return value;
}

std::string Hex(std::span<const uint8_t> bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) { out += digits[b >> 4]; out += digits[b & 15]; }
  return out;
}

std::string Hash(std::span<const uint8_t> bytes) {
  const auto started = std::chrono::steady_clock::now();
  sha256::SHA256 hash;
  if (!bytes.empty()) hash.add(bytes.data(), bytes.size());
  auto result = hash.getHash();
  hash_time += std::chrono::steady_clock::now() - started;
  return result;
}

std::vector<uint8_t> ReadFile(const fs::path& path, uint64_t limit) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) Fail("cannot open patch/package");
  const auto size = file.tellg();
  if (size < 0 || static_cast<uint64_t>(size) > limit) Fail("patch/package exceeds size limit");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  if (!bytes.empty()) file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
  if (!file) Fail("cannot read patch/package");
  return bytes;
}

std::string Lower(std::string value) {
  // The two disc path components are ASCII; do not depend on the host locale.
  for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}

fs::path DiscPath(fs::path root, const char* filename) {
  for (const char* component : {"xuifrontend", filename}) {
    fs::path match;
    for (const auto& entry : fs::directory_iterator(root)) {
      if (Lower(entry.path().filename().string()) == component) {
        if (!match.empty()) Fail("ambiguous case-insensitive menu path");
        match = entry.path();
      }
    }
    if (match.empty()) Fail("PC menu source package missing from game data");
    root = match;
  }
  return root;
}

// A uniquely created directory owns the temporary file. Rename within the
// same filesystem publishes only a completely written and verified package.
struct Staging {
  fs::path path;
  explicit Staging(const fs::path& root) {
    std::random_device random;
    for (int i = 0; i < 16; ++i) {
      auto candidate = root / (".pcmenu-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (fs::create_directory(candidate)) { path = candidate; return; }
    }
    Fail("cannot reserve temporary cache directory");
  }
  ~Staging() { std::error_code ignored; fs::remove_all(path, ignored); }
};

bool Publish(const fs::path& destination, std::span<const uint8_t> bytes, const std::string& sha) {
  // Cache reuse also verifies all bytes; a stale/corrupted cache is replaced.
  if (fs::is_regular_file(destination)) {
    try { if (Hash(ReadFile(destination, kMaxPackage)) == sha) return true; }
    catch (const std::exception&) { /* Rebuild an unreadable/corrupted cache. */ }
  }
  Staging staging(destination.parent_path());
  const auto temporary = staging.path / "menuHUD_3.xzp";
  std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
  if (!out) Fail("cannot create menu cache");
  out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  out.flush();
  if (!out) Fail("cannot write menu cache");
  out.close();
  if (!out || Hash(ReadFile(temporary, kMaxPackage)) != sha) Fail("menu cache verification failed");
#ifdef _WIN32
  // std::filesystem::rename cannot replace a corrupt cache on Windows.
  if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    Fail("cannot replace PC menu cache");
#else
  fs::rename(temporary, destination);
#endif
  return false;
}
}  // namespace

std::vector<uint8_t> ApplyXuiPatch(std::span<const uint8_t> original,
                                 std::span<const uint8_t> patch) {
  if (patch.size() < kHeader || patch.size() > kMaxPatch ||
      std::memcmp(patch.data(), "XUIPAT1\0", 8)) Fail("unsupported/truncated XUIPAT1 patch");
  size_t pos = 8;
  const uint64_t old_size = ReadBE(patch, pos, 8);
  const uint64_t new_size = ReadBE(patch, pos, 8);
  if (old_size > kMaxPackage || new_size > kMaxPackage) Fail("XUIPAT1 package exceeds size limit");
  if (original.size() != old_size || Hash(original) != Hex(patch.subspan(24, 32)))
    Fail("original size/SHA256 mismatch");
  pos = 88;
  const uint64_t count = ReadBE(patch, pos, 4);
  if (count > (patch.size() - kHeader) / 9) Fail("impossible XUIPAT1 operation count");
  std::vector<uint8_t> result;
  result.reserve(static_cast<size_t>(new_size));
  for (uint64_t i = 0; i < count; ++i) {
    const uint64_t kind = ReadBE(patch, pos, 1);
    uint64_t offset = 0;
    if (kind == 0) offset = ReadBE(patch, pos, 8);
    else if (kind != 1) Fail("unknown XUIPAT1 opcode");
    const uint64_t size = ReadBE(patch, pos, 8);
    if (!size || size > new_size - result.size()) Fail("zero/overflowing XUIPAT1 operation");
    if (kind == 0) {
      if (offset > old_size || size > old_size - offset) Fail("COPY exceeds original bounds");
      result.insert(result.end(), original.begin() + offset, original.begin() + offset + size);
    } else {
      if (size > patch.size() - pos) Fail("truncated INSERT");
      result.insert(result.end(), patch.begin() + pos, patch.begin() + pos + size);
      pos += static_cast<size_t>(size);
    }
  }
  if (pos != patch.size() || result.size() != new_size) Fail("trailing bytes/incorrect XUIPAT1 result size");
  if (Hash(result) != Hex(patch.subspan(56, 32))) Fail("result size/SHA256 mismatch");
  return result;
}

void DisablePCMenu(std::string_view reason) {
  guest_path.store(0, std::memory_order_release);
  scene_path.store(0, std::memory_order_release);
  if (!warned.exchange(true)) REXLOG_WARN("pc menu: disabled; using original menu ({})", reason);
}

uint32_t PCMenuGuestPath() {
  // Startup choice only. Do not read mutable cvar storage from guest threads;
  // settings edits take effect after restart, as declared by the lifecycle.
  return guest_path.load(std::memory_order_acquire);
}

uint32_t PCSceneGuestPath() {
  return scene_path.load(std::memory_order_acquire);
}

void PreparePCMenu(rex::Runtime* runtime, const fs::path& game_root, const fs::path& cache_root) {
  if (!REXCVAR_GET(pc_menu_mod)) { REXLOG_INFO("pc menu: disabled by --pc_menu_mod=false"); return; }
  uint32_t allocations[2]{};
  bool mounted = false;
  bool linked = false;
  auto* vfs = runtime->file_system();
  try {
    if (cache_root.empty()) Fail("empty cache root");
    const auto folder = fs::absolute(cache_root) / "xui";
    fs::create_directories(folder);
    std::string guests[2];
    const char* names[] = {"menuHUD_3", "Scene"};
    const char* source_names[] = {"menuhud_3.xzp", "scene.xzp"};
    for (size_t i = 0; i < 2; ++i) {
      const auto started = std::chrono::steady_clock::now();
      hash_time = {};
      const auto original = ReadFile(DiscPath(game_root, source_names[i]), kMaxPackage);
      const auto patch = ReadFile(rex::filesystem::GetExecutableFolder() /
                                 (std::string("xui/") + names[i] + ".xpatch"), kMaxPatch);
      // Reconstruct even on cache reuse: malformed/literal-corrupt patches must
      // fail verification, not be hidden by a previously valid cached result.
      const auto result = ApplyXuiPatch(original, patch);
      const auto sha = Hex(std::span<const uint8_t>(patch).subspan(56, 32));
      const std::string filename = std::string(names[i]) + "." + sha.substr(0, 8) + ".xzp";
      const bool reused = Publish(folder / filename, result, sha);
      const std::string guest = "pcmod:\\" + filename;
      guests[i] = guest;
      allocations[i] = runtime->memory()->SystemHeapAlloc(static_cast<uint32_t>(guest.size() + 1));
      if (!allocations[i]) Fail("PC menu guest path allocation failed");
      std::memcpy(runtime->memory()->TranslateVirtual(allocations[i]), guest.c_str(), guest.size() + 1);
      const auto original_sha = Hex(std::span<const uint8_t>(patch).subspan(24, 32));
      REXLOG_INFO("pc menu: ready {} (original SHA256={}, result SHA256={})", guest, original_sha, sha);
      REXLOG_INFO("pc menu: {} cache {}; SHA256 {:.2f} ms; preparation {:.2f} ms ({} bytes)",
                  names[i], reused ? "reused" : "rebuilt",
                  std::chrono::duration<double, std::milli>(hash_time).count(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                  original.size());
    }
    // Separate read-only device: no game: remount, writes, or SDK changes.
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(kDevice, folder, true);
    if (!device->Initialize()) Fail("PC menu device initialization failed");
    if (!vfs->RegisterDevice(std::move(device))) Fail("PC menu device registration failed");
    mounted = true;
    if (!vfs->RegisterSymbolicLink("pcmod:", kDevice)) Fail("PC menu link registration failed");
    linked = true;
    for (const auto& guest : guests)
      if (!vfs->ResolvePath(guest)) Fail("PC menu guest path not reachable");
    // Publish both paths together only after both verified packages are ready.
    scene_path.store(allocations[1], std::memory_order_release);
    guest_path.store(allocations[0], std::memory_order_release);
    REXLOG_INFO("pc menu: native PC Settings routing enabled (screen 26; unverified at runtime)");
  } catch (const std::exception& error) {
    for (auto allocation : allocations) if (allocation) runtime->memory()->SystemHeapFree(allocation);
    if (linked) vfs->UnregisterSymbolicLink("pcmod:");
    if (mounted) vfs->UnregisterDevice(kDevice);
    DisablePCMenu(error.what());
  }
}
}  // namespace crackdown

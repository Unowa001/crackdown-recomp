#pragma once
#include "core.h"
#include <span>

namespace launcher {
// TODO: owner must confirm repository name and visibility before publication.
inline constexpr char release_repository[] = "Unowa001/crackdown-recomp";
using Bytes = std::vector<unsigned char>;
struct XzpEntry { std::string name; Bytes data; };
std::vector<XzpEntry> read_xzp(std::span<const unsigned char> bytes);
const Bytes* xzp_entry(const std::vector<XzpEntry>& entries, const std::string& name);
Bytes read_bytes(const fs::path& path, uint64_t limit = 128 * 1024 * 1024);
void write_bytes(const fs::path& path, std::span<const unsigned char> bytes);

// Binary KeyValues are preserved, including unrelated Steam shortcut fields.
struct VdfNode { unsigned char type=0; std::string key, value; std::vector<VdfNode> children; };
std::vector<VdfNode> read_vdf(std::span<const unsigned char> bytes);
Bytes write_vdf(const std::vector<VdfNode>& nodes);
bool append_steam_shortcut(std::vector<VdfNode>& nodes, const fs::path& executable);
std::string add_to_steam(const fs::path& executable);

struct Release { std::string tag, notes, url, zip_url, sha_url; bool prerelease=false; };
std::vector<Release> parse_releases(const std::string& json);
// Strict SemVer, optional leading v. Build metadata does not affect ordering.
int compare_versions(const std::string& a, const std::string& b);
void verify_download(const fs::path& zip, const std::string& checksum);
bool update_path_allowed(const std::string& relative);
// Complete package allowlist, regular files only. Roll back on errors; preserve user data.
void replace_staged(const fs::path& package, const fs::path& staging,
                    const std::function<void(size_t)>& before_copy = {});
void recover_update(const fs::path& package);
void rollback_completed_update(const fs::path& package);
// WinHTTP only; explicit caller consent is required. POSIX throws without networking.
std::vector<Release> check_releases(bool beta, const std::atomic_bool& cancel);
void install_release(const Release& release, const fs::path& package, const std::atomic_bool& cancel);
void restart_launcher(const fs::path& package);
}

#include "core.h"
#include "platform.h"
#include "sha256.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace launcher {
namespace {
constexpr uint64_t sector = 2048;
constexpr char magic[] = "MICROSOFT*XBOX*MEDIA";
void check_cancel(const std::atomic_bool* c) { if (c && c->load()) throw std::runtime_error("Cancelled"); }
std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
uint32_t u32(const unsigned char* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t u16(const unsigned char* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
void range(uint64_t off, uint64_t size, uint64_t length) {
  if (off > length || size > length - off) throw std::runtime_error("XDVDFS entry points outside the image");
}
void read(std::ifstream& in, uint64_t off, void* out, size_t size, uint64_t length) {
  range(off, size, length);
  in.clear(); in.seekg(static_cast<std::streamoff>(off));
  if (!in.read(static_cast<char*>(out), static_cast<std::streamsize>(size))) throw std::runtime_error("Cannot read image");
}
std::string hash_range(const fs::path& path, uint64_t off, uint64_t size, const std::atomic_bool* cancel,
                       const std::function<void(uint64_t)>& hashed = {}) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("Cannot read " + utf8(path));
  in.seekg(static_cast<std::streamoff>(off));
  sha256::SHA256 hash;
  std::vector<char> buf(4 * 1024 * 1024);
  uint64_t done = 0;
  while (size) {
    check_cancel(cancel);
    auto n = static_cast<size_t>(std::min<uint64_t>(size, buf.size()));
    if (!in.read(buf.data(), n)) throw std::runtime_error("Short read hashing " + utf8(path));
    hash.add(buf.data(), n); size -= n; done += n;
    if (hashed) hashed(done);
  }
  check_cancel(cancel);
  return hash.getHash();
}
// Cleanup must not replace the error that caused it.
void remove_quietly(const fs::path& path) { std::error_code ec; fs::remove_all(path, ec); }
void no_links(const fs::path& path) {
  if (fs::is_symlink(fs::symlink_status(path))) throw std::runtime_error("Symlink is not permitted: " + utf8(path));
#ifdef _WIN32
  auto attr = GetFileAttributesW(path.c_str());
  if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
    throw std::runtime_error("Reparse point is not permitted: " + utf8(path));
#endif
}
fs::path checked_file(const fs::path& root, const std::string& relative) {
  auto p = root; no_links(p);
  for (const auto& part : fs::u8path(relative)) { p /= part; no_links(p); }
  return p;
}
}
std::string utf8(const fs::path& path) { auto s = path.u8string(); return {s.begin(), s.end()}; }
std::string hash_file(const fs::path& path, const std::atomic_bool* cancel, const std::function<void(uint64_t)>& hashed) {
  return hash_range(path, 0, fs::file_size(path), cancel, hashed);
}
bool safe_name(const std::string& name) {
  if (name.empty() || name.size() > 240 || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
  for (unsigned char c : name) if (c < 32 || c >= 127 || std::string("<>:\"/\\|?*").find(c) != std::string::npos) return false;
  auto stem = lower(name.substr(0, name.find('.')));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
  if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9') return false;
  return true;
}
Image::Image(const fs::path& path) : path_(path), length_(fs::file_size(path)) {
  std::ifstream in(path, std::ios::binary);
  std::array<unsigned char, 2048> descriptor{};
  bool found = false;
  for (uint64_t candidate : {0ULL, 0xFD90000ULL, 0x2080000ULL, 0x18300000ULL}) {
    if (candidate + 0x10000 + sector > length_) continue;
    read(in, candidate + 0x10000, descriptor.data(), descriptor.size(), length_);
    if (std::equal(std::begin(magic), std::end(magic)-1, descriptor.begin()) &&
        std::equal(std::begin(magic), std::end(magic)-1, descriptor.end()-20)) {
      if (candidate == 0x2080000) throw std::runtime_error("XGD3 images are unsupported; select the supported Crackdown XGD2 disc");
      partition_ = candidate; found = true; break;
    }
  }
  if (!found) throw std::runtime_error("No XDVDFS game partition found; select a full Xbox 360 ISO or trimmed XISO");
  struct Dir { uint32_t sector, size; std::string path; unsigned depth; };
  std::vector<Dir> dirs{{u32(descriptor.data()+20), u32(descriptor.data()+24), "", 0}};
  std::set<uint32_t> dir_sectors;
  std::set<std::string> paths;
  size_t entry_count = 0;
  while (!dirs.empty()) {
    auto d = dirs.back(); dirs.pop_back();
    if (!d.size) continue;
    if (d.depth > 64 || d.size > 64*1024*1024 || !dir_sectors.insert(d.sector).second) throw std::runtime_error("Cyclic or oversized XDVDFS directory");
    std::vector<unsigned char> data(d.size);
    read(in, partition_ + uint64_t(d.sector)*sector, data.data(), data.size(), length_);
    std::vector<uint32_t> stack{0}; std::set<uint32_t> visited;
    std::vector<std::pair<uint32_t,uint32_t>> spans;
    while (!stack.empty()) {
      auto off = stack.back(); stack.pop_back();
      if (++entry_count > 100000 || !visited.insert(off).second) throw std::runtime_error("Cyclic XDVDFS directory tree");
      range(off, 14, data.size()); auto p = data.data()+off;
      unsigned n = p[13]; range(off+14, n, data.size());
      uint32_t end = off+14+n;
      for (auto [a,b] : spans) if (off < b && a < end) throw std::runtime_error("Overlapping XDVDFS directory entries");
      spans.emplace_back(off,end);
      std::string name(reinterpret_cast<const char*>(p+14), n);
      if (!safe_name(name)) throw std::runtime_error("Unsafe file name in image: " + name);
      std::string rel = d.path + name;
      if (!paths.insert(lower(rel)).second) throw std::runtime_error("Duplicate file name in image: " + rel);
      uint32_t start = u32(p+4), size = u32(p+8);
      range(partition_ + uint64_t(start)*sector, size, length_);
      if (p[12] & 0x10) dirs.push_back({start,size,rel+"/",d.depth+1});
      else files_.push_back({rel,partition_+uint64_t(start)*sector,size,{}});
      for (unsigned pos : {0,2}) if (uint16_t child = u16(p+pos)) stack.push_back(uint32_t(child)*4);
    }
  }
  std::sort(files_.begin(), files_.end(), [](const Entry& a, const Entry& b) { return lower(a.path) < lower(b.path); });
}
std::vector<Entry> load_manifest(const fs::path& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("Missing game-manifest.txt next to the launcher");
  std::vector<Entry> result; std::set<std::string> names; std::string line;
  while (std::getline(in,line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    auto a = line.find('\t'), b = line.find('\t', a == line.npos ? 0 : a+1);
    if (a == line.npos || b == line.npos) throw std::runtime_error("Invalid manifest row");
    std::string rel = line.substr(0,a), number = line.substr(a+1,b-a-1), hash = line.substr(b+1);
    if (number.empty() || number.find_first_not_of("0123456789") != number.npos) throw std::runtime_error("Invalid manifest size");
    if (rel.empty() || rel.front() == '/' || rel.back() == '/') throw std::runtime_error("Invalid manifest path");
    std::stringstream parts(rel); std::string part;
    while (std::getline(parts,part,'/')) if (!safe_name(part)) throw std::runtime_error("Unsafe manifest path");
    if (!names.insert(lower(rel)).second || (hash != "-" && (hash.size()!=64 || hash.find_first_not_of("0123456789abcdef")!=hash.npos))) throw std::runtime_error("Invalid manifest hash or duplicate path");
    result.push_back({rel,0,std::stoull(number),hash == "-" ? "" : hash});
  }
  if (result.empty() || !names.contains("default.xex")) throw std::runtime_error("Manifest has no default.xex");
  return result;
}
void Image::validate(const std::vector<Entry>& manifest) const {
  if (files_.size() != manifest.size()) throw std::runtime_error("Disc file count does not match the supported version");
  std::map<std::string,uint64_t> sizes;
  for (const auto& e : files_) sizes[lower(e.path)] = e.size;
  for (const auto& e : manifest) if (!sizes.contains(lower(e.path)) || sizes.at(lower(e.path))!=e.size) throw std::runtime_error("Missing or wrong-sized disc file: " + e.path);
}
std::string Image::xex_hash(const std::atomic_bool* cancel) const {
  for (const auto& e : files_) if (lower(e.path)=="default.xex") return hash_range(path_,e.offset,e.size,cancel);
  throw std::runtime_error("Image has no default.xex");
}
std::string verify_folder(const fs::path& folder, const std::vector<Entry>& manifest, const std::atomic_bool* cancel,
                          Check check, Progress progress, const std::string& expected_xex) {
  auto is_xex = [](const Entry& e) { return lower(e.path) == "default.xex"; };
  auto hashed = [&](const Entry& e) { return !is_xex(e) && (check == Check::full || e.size <= quick_hash_limit); };
  uint64_t total = 0, done = 0;
  for (const auto& e : manifest) if (hashed(e)) {
    if (e.hash.empty()) throw std::runtime_error("The manifest has no hash for " + e.path + "; update game-manifest.txt");
    total += e.size;
  }
  std::string xex;
  for (const auto& e : manifest) if (is_xex(e)) {
    auto p=checked_file(folder,e.path);
    if (!fs::is_regular_file(p)) throw std::runtime_error("Missing game file: " + e.path);
    xex=hash_file(p,cancel);
    if (xex!=expected_xex) throw std::runtime_error("Unsupported default.xex version. SHA256: " + xex);
  }
  if (xex.empty()) throw std::runtime_error("Game folder has no default.xex");
  // Sizes first: a missing or truncated file fails in seconds, before any long hashing.
  for (const auto& e : manifest) {
    check_cancel(cancel); auto p = checked_file(folder,e.path);
    if (!fs::is_regular_file(p) || fs::file_size(p)!=e.size) throw std::runtime_error("Missing or wrong-sized game file: " + e.path);
  }
  if (progress) progress(0,total);
  for (const auto& e : manifest) if (hashed(e)) {
    auto report = [&](uint64_t n) { if (progress) progress(done + n, total); };
    if (hash_file(checked_file(folder,e.path),cancel,report)!=e.hash) throw std::runtime_error("Game file is damaged (hash mismatch): " + e.path);
    done += e.size;
  }
  if (progress) progress(done,total);
  return xex;
}
void Image::extract(const fs::path& destination, const std::atomic_bool& cancel, Progress progress) const {
  if (fs::exists(destination)) throw std::runtime_error("Extraction destination already exists");
  no_links(destination.parent_path());
  uint64_t total = 0; for (const auto& e : files_) total += e.size;
  if (fs::space(destination.parent_path()).available < total + total/20) throw std::runtime_error("Not enough disk space (game size plus 5% required)");
  if (!fs::create_directory(destination)) throw std::runtime_error("Cannot create extraction destination");
  try {
    mark_import(destination);
    std::ifstream in(path_, std::ios::binary); std::vector<char> buf(4*1024*1024); uint64_t done=0;
    for (const auto& e : files_) {
      check_cancel(&cancel);
      auto outpath = destination / fs::u8path(e.path);
      fs::create_directories(outpath.parent_path());
      std::ofstream out(outpath, std::ios::binary);
      if (!out) throw std::runtime_error("Cannot write " + utf8(outpath));
      uint64_t pos=0;
      while (pos<e.size) {
        check_cancel(&cancel); size_t n=static_cast<size_t>(std::min<uint64_t>(buf.size(),e.size-pos));
        read(in,e.offset+pos,buf.data(),n,length_);
        if (!out.write(buf.data(),n)) throw std::runtime_error("Failed writing game files");
        done+=n; pos+=n; if (progress) progress(done,total);
      }
      out.close(); if (!out) throw std::runtime_error("Failed closing game file");
    }
  } catch (...) { remove_quietly(destination); throw; }
}
void install_image(const fs::path& iso, const fs::path& home, const std::vector<Entry>& manifest,
                   const std::atomic_bool& cancel, Progress extracted, Progress verified) {
  if (fs::exists(home/"game")) throw std::runtime_error("A game folder already exists. Select that folder or move it before importing an ISO");
  Image image(iso);
  auto hash=image.xex_hash(&cancel);
  if (hash!=supported_xex) throw std::runtime_error("Unsupported default.xex version. SHA256: " + hash);
  image.validate(manifest); check_cancel(&cancel);
  auto staging=home/("game-install-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  image.extract(staging,cancel,extracted);
  try {
    verify_folder(staging,manifest,&cancel,Check::full,verified);
    std::ofstream stamp(staging/".verified"); stamp << hash_file(home/"game-manifest.txt") << '\n' << hash << '\n';
    stamp.close(); if (!stamp) throw std::runtime_error("Cannot write verification receipt");
    check_cancel(&cancel);
    if (fs::exists(home/"game")) throw std::runtime_error("Game folder appeared during extraction");
    if (!fs::remove(staging/".crackdown-import")) throw std::runtime_error("Cannot remove import marker");
    fs::rename(staging,home/"game");
  } catch (...) { remove_quietly(staging); throw; }
}
void mark_import(const fs::path& folder) {
  auto owner=current_process_owner();
  std::ofstream out(folder/".crackdown-import",std::ios::binary);
  out << "crackdown-import-v1\n" << owner.pid << ' ' << owner.started << '\n';
  out.close(); if (!out) throw std::runtime_error("Cannot write import ownership marker");
}
std::vector<fs::path> unfinished_imports(const fs::path& home, const fs::path& selected) {
  std::vector<fs::path> result; std::error_code ec;
  auto protected_path=selected.empty()?fs::path{}:fs::weakly_canonical(selected,ec);
  if(ec) return result; // Uncertain selection: do not offer destructive cleanup.
  for (const auto& entry : fs::directory_iterator(home, ec)) {
    if (!utf8(entry.path().filename()).starts_with("game-install-")) continue;
    try {
      no_links(entry.path());
      if (!entry.is_directory()) continue;
      auto canonical=fs::canonical(entry.path());
      if (!protected_path.empty() && (canonical==protected_path || fs::equivalent(canonical,protected_path,ec))) continue;
      auto marker=entry.path()/".crackdown-import"; no_links(marker);
      if (!fs::is_regular_file(marker)) continue;
      std::ifstream in(marker,std::ios::binary); std::string version, extra; ProcessOwner owner{};
      if (!(in >> version >> owner.pid >> owner.started) || version!="crackdown-import-v1" ||
          !owner.pid || !owner.started || (in >> extra)) continue;
      if (!owner_alive(owner)) result.push_back(entry.path());
    } catch (const std::exception&) { /* Unreadable/untrusted folders are never cleanup candidates. */ }
  }
  return result;
}
bool valid_profile_name(const std::string& name) {
  return name.size() <= 40 && safe_name(name) && name.front() != '.' && name.front() != ' ' && name.front() != '$';
}
std::vector<std::string> list_profiles(const fs::path& user) {
  std::vector<std::string> result; std::error_code ec;
  try { no_links(user/"profiles"); } catch (const std::exception&) { return result; }
  for (const auto& entry : fs::directory_iterator(user/"profiles", ec)) {
    auto name = utf8(entry.path().filename());
    try { no_links(entry.path()); if (entry.is_directory(ec) && valid_profile_name(name)) result.push_back(name); }
    catch (const std::exception&) {}
  }
  std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) { return lower(a) < lower(b); });
  return result;
}
void create_profile(const fs::path& user, const std::string& name) {
  if (!valid_profile_name(name)) throw std::runtime_error("Use up to 40 letters, digits, spaces, dots, dashes or underscores");
  for (const auto& existing : list_profiles(user))
    if (lower(existing) == lower(name)) throw std::runtime_error("A profile with that name already exists");
  no_links(user/"profiles");
  fs::create_directories(user/"profiles");
  no_links(user/"profiles");
  if (!fs::create_directory(user/"profiles"/fs::u8path(name))) throw std::runtime_error("Cannot create the profile folder");
}
fs::path profile_folder(const fs::path& user, const std::string& name) {
  auto saves=fs::absolute(user).lexically_normal();
  if (!name.empty()) {
    if (!valid_profile_name(name)) throw std::runtime_error("Save profile not found: " + name);
    no_links(saves/"profiles");
    saves=saves/"profiles"/fs::u8path(name); no_links(saves);
    if (!fs::is_directory(saves)) throw std::runtime_error("Save profile not found: " + name);
  }
  return saves;
}
std::vector<std::string> profile_args(const fs::path& user, const std::string& name) {
  auto root=fs::absolute(user).lexically_normal();
  return {"--user_data_root=" + utf8(profile_folder(user, name)), "--cache_root=" + utf8(root/"cache")};
}
static fs::path ap_folder(const fs::path& saves) { no_links(saves/"archipelago"); return saves/"archipelago"; }
ApConnection load_ap_connection(const fs::path& saves) {
  auto t=read_config(ap_folder(saves)/"connection.toml");
  auto text=[&](const char* key) { return t[key].value<std::string>().value_or(""); };
  return {t["enabled"].value<bool>().value_or(false), text("server"), text("slot"), text("password")};
}
void save_ap_connection(const fs::path& saves, const ApConnection& c) {
  auto dir=ap_folder(saves); fs::create_directories(dir); no_links(dir);
  auto text=[](const std::string& s) { return s.empty()?std::nullopt:std::optional<std::string>(toml_string(s)); };
  update_config(dir/"connection.toml", {{"enabled", c.enabled?"true":"false"}, {"server", text(c.server)},
                                        {"slot", text(c.slot)}, {"password", text(c.password)}});
}
static std::string trimmed(const std::string& s) {
  auto b=s.find_first_not_of(" \t"), e=s.find_last_not_of(" \t");
  return b==std::string::npos?std::string():s.substr(b, e-b+1);
}
std::vector<std::string> ap_args(const ApConnection& c) {
  // Never an empty value: the SDK's CLI11 rejects "--name=" and then drops the whole command
  // line, profile root included (beta 0.3.0). The launcher strips ap_* from crackdown.toml
  // instead (kApConfigKeys), and with no --ap_password the game reads connection.toml.
  if (!c.enabled) return {};
  auto server=trimmed(c.server), slot=trimmed(c.slot);
  if (server.empty()) throw std::runtime_error("Enter the Archipelago server (host:port) or turn Archipelago off");
  if (slot.empty()) throw std::runtime_error("Enter your Archipelago slot name or turn Archipelago off");
  if (server.find_first_of(" \t\"") != std::string::npos) throw std::runtime_error("The Archipelago server address cannot contain spaces or quotes");
  return {"--ap_server=" + server, "--ap_slot=" + slot};
}
static bool valid_ipv6(const std::string& text) {
  unsigned char address[16];
  return !text.empty() && text.find('%')==std::string::npos && inet_pton(AF_INET6,text.c_str(),address)==1;
}
static std::optional<std::string> percent_decoded(const std::string& s) {
  std::string out;
  for (size_t i=0; i<s.size(); ++i) {
    if (s[i]!='%') { out+=s[i]; continue; }
    if (i+2>=s.size() || !std::isxdigit((unsigned char)s[i+1]) || !std::isxdigit((unsigned char)s[i+2])) return std::nullopt;
    out+=char(std::stoi(s.substr(i+1,2),nullptr,16)); i+=2;
  }
  // No control characters (NUL would truncate the value in the UI and in connection.toml).
  for (unsigned char ch : out) if (ch<0x20 || ch==0x7f) return std::nullopt;
  return out;
}
std::optional<ApConnection> parse_ap_uri(const std::string& uri) {
  const std::string scheme="archipelago://";
  if (uri.size()<=scheme.size() || lower(uri.substr(0,scheme.size()))!=scheme) return std::nullopt;
  for (unsigned char ch : uri) if (ch<0x20 || ch==0x7f) return std::nullopt;
  auto rest=uri.substr(scheme.size());
  auto end=rest.find_first_of("/?#");
  // A '@' after the authority means the credentials were not encoded; refuse rather than read
  // "slot:pass/word@host" as the host "slot" with the password digits as its port.
  if (end!=std::string::npos && rest.find('@',end)!=std::string::npos) return std::nullopt;
  rest=rest.substr(0,end);
  ApConnection c; c.enabled=true;
  if (std::count(rest.begin(),rest.end(),'@')>1) return std::nullopt;  // credentials must encode '@'
  if (auto at=rest.find('@'); at!=std::string::npos) {
    auto user=rest.substr(0,at); rest=rest.substr(at+1);
    auto colon=user.find(':');
    auto slot=percent_decoded(user.substr(0,colon));
    auto password=colon==std::string::npos?std::optional<std::string>(""):percent_decoded(user.substr(colon+1));
    if (!slot || !password) return std::nullopt;
    c.slot=trimmed(*slot); c.password=*password;
  }
  if (rest.empty() || rest.find_first_of(" 	\"@%") != std::string::npos) return std::nullopt;
  // Host: "[ipv6]" or a name/IPv4 without brackets or colons, then an optional ":port".
  size_t host_end;
  if (rest[0]=='[') {
    host_end=rest.find(']');
    if (host_end==std::string::npos || rest.find_first_of("[]",host_end+1)!=std::string::npos ||
        !valid_ipv6(rest.substr(1,host_end-1))) return std::nullopt;
    ++host_end;
  } else {
    host_end=rest.find(':');
    if (host_end==std::string::npos) host_end=rest.size();
    if (host_end==0 || rest.find_first_of("[]")!=std::string::npos) return std::nullopt;
  }
  if (host_end==rest.size()) rest+=":38281";
  else {
    if (rest[host_end]!=':') return std::nullopt;
    auto port=rest.substr(host_end+1);
    if (port.empty() || port.size()>5 || port.find_first_not_of("0123456789")!=std::string::npos) return std::nullopt;
    if (auto n=std::stoi(port); n<1 || n>65535) return std::nullopt;
  }
  c.server=rest;
  return c;
}
static void redact(toml::table& table) {
  std::vector<std::string> secrets;
  for (auto&& [key, node] : table) {
    if (lower(std::string(key.str())).find("password") != std::string::npos) secrets.emplace_back(key.str());
    else if (auto* child = node.as_table()) redact(*child);
    else if (auto* array = node.as_array())
      for (auto& item : *array) if (auto* t = item.as_table()) redact(*t);
  }
  for (const auto& key : secrets) table.insert_or_assign(key, "(redacted)");
}
std::string redacted_config(const fs::path& config) {
  try {
    auto table = read_config(config);
    redact(table);
    std::ostringstream out; out << toml::toml_formatter{table}; return out.str();
  } catch (const std::exception&) { return "(could not be parsed; not included)\n"; }
}
std::string ap_binding(const fs::path& saves) {
  auto sidecar=ap_folder(saves)/"session.toml";
  if (!fs::exists(sidecar)) return {};
  try {
    auto t=read_config(sidecar);
    return "seed " + t["seed"].value<std::string>().value_or("?") + ", slot " +
           std::to_string(t["slot"].value<int64_t>().value_or(0));
  } catch (const std::exception&) { return "unreadable session file"; }
}
}

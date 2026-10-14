// In-process Archipelago bridge (M2: native checks, permanent items and saved goal).
//
// One coordinator, guarded by g_mutex, owns SessionState. The network thread
// (client.cpp) only queues packets; they are applied here on the game thread
// from the existing frame hook (hooks_framerate.cpp, sub_826A7698).
//
// Guest contracts (docs/re-ap-probe.md, docs/re-ap-bridge.md):
// - 824EEEE8 constructs the pickup manager: once at boot and once per world
//   (re)load, so it marks a save/load/new-game transition.
// - 824EF158 (pickup manager vtable 820B3B34) deserializes orb availability;
//   called from the native load at LR 8254DB50. A New Game never calls it.
// - 824EF1E0 (vtable 820B3B38) serializes it; stream+76 != 0 on the write pass
//   (LR 8254E61C, from the supply-point save 8268F0A8).
// - 824EF6F8 (on foot) / 824EF588 (vehicle) are the pickup queries, called
//   only from the collection path 82313968 with r5 = player (r28). A non-null
//   r3 is the selected pickup; the caller clears its +288+player byte
//   unconditionally at 82313D38 (r25 = 0) and queues its XP.
// - 82543A28 / 82543BB0 award bonus / ordinary XP to SkillSet r3.
// - 82543DB0 sets an absolute skill level: r3 SkillSet, r4 skill, r5 raw
//   level, r6 = 1 (as console callback 825087A8 calls it).
#include "generated/default/crackdown_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <map>
#include <random>
#include <set>
#include <array>
#include <toml++/toml.hpp>

#include "../input_replay.h"
#include "bridge.h"
#include "catalog.h"
#include "client.h"
#include "session_state.h"
#include "sha256.h"
#include "adapter_policy.h"

REXCVAR_DEFINE_STRING(ap_server, "", "Archipelago",
                      "Archipelago server (host:port, ws://... or wss://...); empty = offline");
REXCVAR_DEFINE_STRING(ap_slot, "", "Archipelago", "Archipelago slot (player) name");
REXCVAR_DEFINE_STRING(ap_password, "", "Archipelago",
                      "Archipelago room password; prefer the launcher, which keeps it in "
                      "<user_data>/archipelago/connection.toml (cvar values can reach debug logs)");
REXCVAR_DEFINE_STRING(ap_dev_fault, "", "Archipelago",
                      "Developer fault injection: duplicate_items, gap_items, fail_observe, "
                      "fail_observe_always, fail_begin_save, fail_record_save "
                      "(exercises resync/retry/recovery paths)");

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace crackdown::ap {
namespace {
constexpr uint32_t kTitle = 0x4D5307DC;
constexpr uint32_t kPlayerGlobal = 0x82DE3F3C;
constexpr uint32_t kPickupManagerGlobal = 0x82DE3F34;
constexpr uint32_t kNativeLoadLR = 0x8254DB50;
constexpr uint32_t kNativeSaveLR = 0x8254E61C;
constexpr const char* kSkillNames[] = {"Agility", "Driving", "Explosives", "Strength", "Firearms"};

enum class Mode { idle, transition, bound, foreign };

struct Bridge {
  bool enabled = false;
  fs::path root, sidecar;
  const Catalog* catalog = nullptr;
  std::unique_ptr<SessionState> session;
  std::optional<Contract> stored;  // contract of an existing sidecar
  std::string halt;                // non-empty: local AP processing stopped
  std::string offline;             // non-empty: this server was rejected; play offline

  Client client;
  uint64_t generation = 0;        // generation of the validated connection
  bool connect_sent = false, server_ready = false;
  std::optional<Contract> server; // validated server contract (profile/save unset)
  Clock::time_point last_check_send{};
  std::map<int, std::string> players;  // slot -> name, for messages

  Mode mode = Mode::idle;
  bool load_seen = false, load_bound = false;
  std::string load_reason;
  std::string load_hash;
  std::vector<int64_t> retry_observe;

  // Native save watch after a bound save pass.
  bool watch_save = false;
  fs::file_time_type watch_stamp{};
  uintmax_t watch_size = 0;
  int watch_stable = 0;
  bool record_failing = false;
  std::set<int64_t> watch_dead;
  bool native_saving = false;
  std::set<int64_t> serialized_bosses, serialized_dead;
  bool watch_maximum_skills = false;
  uint64_t goal_generation = 0;

  // Readiness: character updates advancing for >= 1 s.
  uint64_t last_updates = 0;
  Clock::time_point steady_since{}, last_grant_try{}, last_observe_retry{};
  bool steady = false;
  uint64_t frame = 0;
};
std::mutex g_mutex;
Bridge g;
bool g_fault_used = false;

// One-shot unless the name ends in "_always".
bool Fault(const char* name) {
  const auto fault = REXCVAR_GET(ap_dev_fault);
  if (fault == std::string(name) + "_always") return true;
  if (fault != name || g_fault_used) return false;
  g_fault_used = true;
  REXLOG_WARN("ap: dev fault injected: {}", name);
  return true;
}

const Binding& ActiveBinding() { return g.session->binding(); }

// Local failure (sidecar, binding, guest contract): stop all AP processing.
void Halt(const std::string& why) {
  if (!g.halt.empty()) return;
  g.halt = why;
  REXLOG_ERROR("ap: HALTED: {}. AP processing stopped; restart after fixing the cause.", why);
  g.server_ready = false;
  g.client.RequestStop();
}

// Server-side rejection (wrong seed/slot/password, invalid packet): stop talking
// to this server only. A bound save stays this profile's randomized save and
// keeps its rules offline; checks persist and are sent on a later connection.
void Disconnect(const std::string& why) {
  if (!g.offline.empty()) return;
  g.offline = why;
  REXLOG_ERROR("ap: server rejected: {}. Playing offline; restart with the right server to sync.", why);
  g.server_ready = false;
  g.generation = 0;
  g.client.RequestStop();
}

// --ap_password if given, else the password the launcher keeps in this profile's
// archipelago/connection.toml. Read here rather than passed as a cvar, so it never
// reaches argv, the environment or the SDK's cvar debug logging.
std::string Password() {
  if (auto cvar = REXCVAR_GET(ap_password); !cvar.empty()) return cvar;
  try {
    const auto path = g.root / "archipelago" / "connection.toml";
    if (!fs::exists(path)) return {};
    return toml::parse_file(path.string())["password"].value<std::string>().value_or("");
  } catch (const std::exception& e) {
    REXLOG_WARN("ap: cannot read the saved connection settings: {}", e.what());
    return {};
  }
}

// One id per profile, kept across connections (AP Connect.uuid).
std::string NewSaveUuid();

// Kept in memory too, so an unwritable profile still reuses one id per run.
std::string ClientUuid() {
  static std::string uuid;
  if (!uuid.empty()) return uuid;
  const auto path = g.root / "archipelago" / "client_uuid";
  if (std::ifstream in{path}; in >> uuid && uuid.size() == 32) return uuid;
  uuid = NewSaveUuid();
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  std::ofstream out(path);
  if (!(out << uuid << '\n'))
    REXLOG_WARN("ap: cannot save the client id to {}; using it for this run only", path.string());
  return uuid;
}

std::optional<int64_t> WriteTime(const fs::path& path) {
  std::error_code ec;
  const auto stamp = fs::last_write_time(path, ec);
  if (ec) return std::nullopt;
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::clock_cast<std::chrono::system_clock>(stamp).time_since_epoch())
      .count();
}

int64_t Now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Profile directory (XUID) that holds this title's content.
std::optional<std::string> ProfileXuid() {
  std::error_code ec;
  std::optional<std::string> found;
  for (const auto& entry : fs::directory_iterator(g.root, ec)) {
    const auto name = entry.path().filename().string();
    if (name.size() != 16 || !entry.is_directory()) continue;
    if (!fs::exists(entry.path() / "4D5307DC", ec)) continue;
    if (found) return std::nullopt;  // ambiguous: refuse to guess
    found = name;
  }
  return found;
}

fs::path SavePath(const std::string& xuid) {
  return g.root / xuid / "4D5307DC" / "00000001" / "Crackdown.sav" / "Crackdown.sav";
}

std::optional<std::string> HashFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  sha256::SHA256 hash;
  char buffer[65536];
  while (in) {
    in.read(buffer, sizeof(buffer));
    if (in.gcount() > 0) hash.add(buffer, size_t(in.gcount()));
  }
  if (in.bad()) return std::nullopt;
  return hash.getHash();
}

std::string NewSaveUuid() {
  std::random_device random;
  char text[33];
  for (int i = 0; i < 4; ++i) std::snprintf(text + 8 * i, 9, "%08x", random());
  return text;
}

// Primary agent SkillSet, using the supported-build layout guard from ap_probe.
uint32_t PrimarySkills(uint8_t* base) {
  const uint32_t player = REX_LOAD_U32(kPlayerGlobal);
  if (!player) return 0;
  const uint32_t component = REX_LOAD_U32(player + 292);
  if (!component || component < 32) return 0;
  const uint32_t character = component - 32;
  const uint32_t vtable = REX_LOAD_U32(character);
  if (vtable != 0x8208D850 || REX_LOAD_U32(vtable + 1220) != 0x82299350) return 0;
  const uint32_t skills = character + 9056;
  return REX_LOAD_U32(skills + 8) == character ? skills : 0;
}

bool SaveIsBound() { return g.mode == Mode::bound && g.session; }

uint32_t Armory(uint8_t* base) {
  const uint32_t manager = REX_LOAD_U32(0x835D3F84);
  if (!manager || g.catalog->weapons.size() != 19) return 0;
  const uint32_t begin = REX_LOAD_U32(manager + 12), end = REX_LOAD_U32(manager + 16);
  if (!begin || end < begin || end - begin != 19 * 112) return 0;
  for (const auto& [id, w] : g.catalog->weapons) {
    const uint32_t entry = begin + 112 * w.index;
    if (REX_LOAD_U32(entry + 12) != id || REX_LOAD_U8(entry + 8) != w.starting ||
        REX_LOAD_U8(entry + 9) > 1) return 0;
  }
  return begin;
}

// Resolver calls inside the original carried-firearm/throwable scan provide
// identity even for already available starting/received weapons. No synthetic
// inventory enumeration and no availability transition used as provenance.
struct DepositPass;
thread_local DepositPass* g_deposit = nullptr;
struct DepositPass {
  uint8_t* base;
  uint32_t begin;
  std::array<uint8_t, 19> before{};
  std::set<int> carried;
  bool restored = false;
  void Restore() {
    if (restored) return;
    for (int i = 0; i < 19; ++i) REX_STORE_U8(begin + 112 * i + 9, before[i]);
    restored = true;
    g_deposit = nullptr;
  }
  ~DepositPass() { Restore(); }
};

void ReconcileWeapons(uint8_t* base) {
  if (g.session->contract().weapons.empty()) return;
  const uint32_t begin = Armory(base);
  if (!begin) return Halt("armory layout/catalog mismatch");
  for (const auto& [id, w] : g.catalog->weapons) {
    const auto target = g.session->WeaponTarget(ActiveBinding(), id, true);
    if (!target) continue; // unknown inventory/resync: no grants or revocations
    const uint32_t flag = begin + 112 * w.index + 9;
    const bool native = REX_LOAD_U8(flag) != 0;
    if (native != *target) {
      REX_STORE_U8(flag, *target ? 1 : 0);
      REXLOG_INFO("ap: armory native_id={} available {} -> {}", id, native, *target);
    }
  }
}

// --- network -------------------------------------------------------------

void SendCommands(nlohmann::json commands) {
  if (g.generation) g.client.Send(g.generation, std::move(commands));
}

void SendPendingChecks() {
  if (!g.server_ready || !g.session || !g.halt.empty()) return;
  auto pending = g.session->PendingChecks();
  g.last_check_send = Clock::now();
  if (pending.empty()) return;
  REXLOG_INFO("ap: sending {} pending check(s)", pending.size());
  SendCommands(nlohmann::json::array({{{"cmd", "LocationChecks"}, {"locations", pending}}}));
}

void SendGoal() {
  if (!g.server_ready || !SaveIsBound() || !g.halt.empty() ||
      !g.session->GoalComplete() || g.goal_generation == g.generation) return;
  // CLIENT_GOAL is monotonic/idempotent server state, with no protocol ack.
  // Once per validated connection; replay on reconnect repairs lost sends or
  // server rollback. The durable native-save witness prevents losing victory.
  SendCommands(nlohmann::json::array({{{"cmd", "StatusUpdate"}, {"status", 30}}}));
  g.goal_generation = g.generation;
  REXLOG_INFO("ap: CLIENT_GOAL sent (native save witness, generation {})", g.generation);
}

void RequestResync() {
  REXLOG_WARN("ap: received-items index mismatch; requesting Sync");
  nlohmann::json commands = nlohmann::json::array({{{"cmd", "Sync"}}});
  if (g.session) {
    auto pending = g.session->PendingChecks();
    if (!pending.empty()) commands.push_back({{"cmd", "LocationChecks"}, {"locations", pending}});
  }
  SendCommands(std::move(commands));
}

// Validates Connected against the catalog; returns the server contract.
std::optional<Contract> ValidateConnected(const nlohmann::json& p, const std::string& seed,
                                          std::string& error) {
  try {
    const int slot = p.at("slot").get<int>();
    if (p.at("slot_info").at(std::to_string(slot)).at("game").get<std::string>() != "Crackdown") {
      error = "slot is not a Crackdown slot"; return {};
    }
    std::set<int64_t> locations;
    for (const char* key : {"missing_locations", "checked_locations"})
      for (const auto& id : p.at(key))
        if (!locations.insert(id.get<int64_t>()).second) {
          error = "duplicate server location"; return {};
        }
    auto c = ParseSlotContract(*g.catalog, p.at("slot_data").dump(), locations);
    c.binding.game = "Crackdown";
    c.binding.seed = seed;
    c.binding.team = p.at("team").get<int>();
    c.binding.slot = slot;
    return c;
  } catch (const std::exception& e) {
    error = std::string("malformed Connected/slot_data: ") + e.what();
    return {};
  }
}

std::set<int64_t> CheckedIn(const nlohmann::json& p) {
  std::set<int64_t> out;
  if (p.contains("checked_locations"))
    for (const auto& id : p["checked_locations"]) out.insert(id.get<int64_t>());
  return out;
}

void Acknowledge(const std::set<int64_t>& checked) {
  if (!g.session || checked.empty()) return;
  g.session->Acknowledge(ActiveBinding(), checked);
}

void HandlePacket(const NetEvent& e) {
  const auto& p = e.body;
  if (e.cmd == "RoomInfo") {
    const auto seed = p.value("seed_name", std::string());
    if (seed.empty()) return Disconnect("RoomInfo without seed_name");
    if (g.stored && g.stored->binding.seed != seed)
      return Disconnect("server seed " + seed + " differs from this profile's bound seed " +
                        g.stored->binding.seed);
    if (REXCVAR_GET(ap_slot).empty()) return Disconnect("ap_slot is not set");
    g.generation = e.generation;
    g.connect_sent = true;
    g.server = Contract{};
    g.server->binding.seed = seed;
    nlohmann::json connect = {
        {"cmd", "Connect"}, {"password", Password()}, {"game", "Crackdown"},
        {"name", REXCVAR_GET(ap_slot)}, {"uuid", ClientUuid()},
        {"version", {{"major", 0}, {"minor", 6}, {"build", 0}, {"class", "Version"}}},
        {"items_handling", 7}, {"tags", nlohmann::json::array()}, {"slot_data", true}};
    REXLOG_INFO("ap: room seed {}; connecting as slot '{}'", seed, REXCVAR_GET(ap_slot));
    SendCommands(nlohmann::json::array({connect}));
  } else if (e.cmd == "ConnectionRefused") {
    Disconnect("connection refused: " + p.value("errors", nlohmann::json::array()).dump());
  } else if (e.cmd == "Connected") {
    std::string error;
    auto contract = ValidateConnected(p, g.server ? g.server->binding.seed : std::string(), error);
    if (!contract) return Disconnect(error);
    if (g.stored) {
      auto expected = *g.stored;
      expected.binding.profile.clear();
      expected.binding.save.clear();
      if (!(expected == *contract))
        return Disconnect("server slot/team/catalog/locations differ from this profile's bound session");
    }
    g.server = contract;
    g.server_ready = true;
    g.players.clear();
    for (const auto& player : p.value("players", nlohmann::json::array()))
      if (player.value("team", -1) == contract->binding.team)
        g.players[player.value("slot", 0)] = player.value("alias", player.value("name", std::string()));
    REXLOG_INFO("ap: connected team {} slot {}; {} locations ({} checked on server)",
                contract->binding.team, contract->binding.slot, contract->locations.size(),
                CheckedIn(p).size());
    // Connected is authoritative: a server restored from an older save can
    // forget checks it once acknowledged, and they become pending again
    // (LocationChecks is idempotent on the server). Pending checks are this
    // seed's observations from the bound save, so they are sent even while a
    // foreign or undecided native save is loaded.
    if (g.session) g.session->Acknowledge(ActiveBinding(), CheckedIn(p), true);
    SendPendingChecks();
  } else if (e.cmd == "ReceivedItems") {
    if (!g.session) return;  // not yet bound: the bind requests a full Sync
    std::vector<NetworkItem> items;
    for (const auto& i : p.at("items"))
      items.push_back({i.at("item").get<int64_t>(), i.at("location").get<int64_t>(),
                       i.at("player").get<int>(), i.at("flags").get<int>()});
    auto index = p.at("index").get<int64_t>();
    if (index > 0 && Fault("gap_items")) ++index;
    auto receipt = g.session->Receive(ActiveBinding(), index, items);
    if (receipt == Receipt::appended && Fault("duplicate_items"))
      receipt = g.session->Receive(ActiveBinding(), index, items);
    if (receipt == Receipt::resync_required) RequestResync();
    else
      REXLOG_INFO("ap: received items index {} count {} ({}); inventory {}", index, items.size(),
                  receipt == Receipt::replaced ? "full" : "appended", g.session->Inventory().size());
  } else if (e.cmd == "RoomUpdate") {
    Acknowledge(CheckedIn(p));
  } else if (e.cmd == "PrintJSON") {
    std::string text;
    for (const auto& part : p.value("data", nlohmann::json::array())) {
      if (!part.contains("text") || !part["text"].is_string()) continue;
      auto piece = part["text"].get<std::string>();
      const auto type = part.value("type", std::string());
      try {
        if (type == "player_id" && g.players.contains(std::stoi(piece))) piece = g.players[std::stoi(piece)];
        else if (type == "item_id") {
          const auto id = std::stoll(piece);
          if (id == g.catalog->agility_item) piece = "Progressive Agility";
          for (const auto& [index, skill] : g.catalog->skills)
            if (id == skill.item) piece = std::string("Progressive ") + kSkillNames[index];
        }
        else if (type == "location_id" && g.catalog->orbs.contains(std::stoll(piece)))
          piece = g.catalog->orbs.at(std::stoll(piece)).name;
      } catch (const std::exception&) {
      }
      text += piece;
    }
    if (!text.empty()) REXLOG_INFO("ap: server: {}", text);
  }
}

void PumpNetwork() {
  for (auto& e : g.client.Drain()) {
    if (!g.halt.empty() || !g.offline.empty()) continue;
    if (e.kind == NetEvent::Kind::connected_socket) {
      REXLOG_INFO("ap: socket open (generation {})", e.generation);
      continue;
    }
    if (e.kind == NetEvent::Kind::disconnected) {
      if (e.generation == g.generation || !g.generation) {
        REXLOG_WARN("ap: disconnected: {}; checks stay pending and retry on reconnect", e.detail);
        g.server_ready = false;
        g.generation = 0;
      }
      continue;
    }
    // Packets from an older socket can never touch the current session.
    if (e.cmd != "RoomInfo" && e.generation != g.generation) continue;
    try {
      HandlePacket(e);
    } catch (const std::exception& ex) {
      // Commits are atomic, so a rejected packet changed nothing; the next
      // connection starts from a full Sync.
      Disconnect(std::string("packet ") + e.cmd + " rejected: " + ex.what());
    }
  }
}

// --- native lifecycle -----------------------------------------------------

// Verify the whole orb catalog, including orbs excluded by slot options.
bool VerifySupplies(uint8_t* base) {
  if (g.catalog->supplies.empty()) return true;
  const uint32_t manager = REX_LOAD_U32(0x82DE2678);
  if (!manager || REX_LOAD_U32(manager + 8) != 16) return false;
  for (const auto& [index, s] : g.catalog->supplies) {
    if (REX_LOAD_U8(manager + 2616 + index) > 1) return false;
    for (int axis = 0; axis < 3; ++axis) {
      const float v = std::bit_cast<float>(REX_LOAD_U32(manager + 64 + 64 * index + 4 * axis));
      if (!std::isfinite(v) || std::fabs(v - s.position[axis]) > 0.05f) return false;
    }
  }
  return true;
}

std::optional<std::map<int, uint32_t>> BossTable(uint8_t* base) {
  std::map<int, uint32_t> bosses;
  const uint32_t manager = REX_LOAD_U32(0x82DE27F4);
  if (!manager || g.catalog->bosses.size() != 21 ||
      REX_LOAD_U32(manager + 192) != manager + 208 + 21 * 4) return {};
  for (int i = 0; i < 21; ++i) {
    const uint32_t boss = REX_LOAD_U32(manager + 208 + 4 * i);
    if (!boss || REX_LOAD_U32(boss) != 0x820A513C || REX_LOAD_U8(boss + 148) > 1) return {};
    const int id = static_cast<int>(REX_LOAD_U32(boss + 32));
    if (!g.catalog->bosses.contains(id) || !bosses.emplace(id, boss).second) return {};
  }
  return bosses;
}

std::set<int64_t> NativeDead(uint8_t* base) {
  const auto bosses = BossTable(base);
  if (!bosses) throw std::runtime_error("boss layout/catalog mismatch");
  std::set<int64_t> dead;
  for (const auto& [id, boss] : *bosses)
    if (REX_LOAD_U8(boss + 148) == 1) dead.insert(g.catalog->bosses.at(id).id);
  return dead;
}

bool NativeMaximumSkills(uint8_t* base) {
  const uint32_t skills = PrimarySkills(base);
  if (!skills) return false;
  for (int i = 0; i < 5; ++i) if (REX_LOAD_U8(skills + 17 + i) != 5) return false;
  return true;
}

bool VerifyCatalog(uint8_t* base, std::string& error) {
  const uint32_t manager = REX_LOAD_U32(kPickupManagerGlobal);
  if (!manager || REX_LOAD_U32(manager + 4012) != 800) { error = "pickup manager not loaded"; return false; }
  for (const auto& [id, orb] : g.catalog->orbs) {
    const uint32_t pickup = REX_LOAD_U32(manager + 12 + 4 * orb.ordinal);
    if (!pickup || REX_LOAD_U32(pickup + 264) != orb.type) { error = "orb type mismatch"; return false; }
    for (int axis = 0; axis < 3; ++axis) {
      const float v = std::bit_cast<float>(REX_LOAD_U32(pickup + 272 + 4 * axis));
      if (!std::isfinite(v) || std::fabs(v - orb.position[axis]) > 0.05f) { error = "orb position mismatch"; return false; }
    }
  }
  if (!VerifySupplies(base)) { error = "supply table mismatch"; return false; }
  if (!g.catalog->bosses.empty() && !BossTable(base)) { error = "boss table mismatch"; return false; }
  return true;
}

void Observe(int64_t location) {
  try {
    if (Fault("fail_observe")) throw std::runtime_error("injected sidecar write failure");
    if (g.session->Observe(ActiveBinding(), location)) {
      REXLOG_INFO("ap: check observed location {}", location);
      SendPendingChecks();
    }
  } catch (const std::exception& e) {
    // The native pickup already happened. Keep it in memory and retry; a crash
    // here is recovered from the save's native collected flags on next load.
    REXLOG_ERROR("ap: could not persist check {}: {}; will retry", location, e.what());
    g.retry_observe.push_back(location);
  }
}

// After a bound load: natively collected selected orbs that the sidecar lacks
// (pickup persisted by a native save, observation lost to a crash/write error).
void RecoverNativeChecks(uint8_t* base) {
  const uint32_t manager = REX_LOAD_U32(kPickupManagerGlobal);
  for (auto id : g.session->contract().locations) {
    if (!g.catalog->orbs.contains(id)) continue;
    const uint32_t pickup = REX_LOAD_U32(manager + 12 + 4 * g.catalog->orbs.at(id).ordinal);
    if (REX_LOAD_U8(pickup + 288) == 0) Observe(id);
  }
  const uint32_t supplies = REX_LOAD_U32(0x82DE2678);
  for (const auto& [index, s] : g.catalog->supplies)
    if (!s.starting && g.session->contract().locations.contains(s.id) &&
        REX_LOAD_U8(supplies + 2616 + index) == 1) Observe(s.id);
  if (!g.catalog->bosses.empty())
    for (auto id : NativeDead(base)) if (g.session->contract().locations.contains(id)) Observe(id);
}

bool BindNewGame(std::string& reason) {
  if (g.session) {
    if (g.session->HasNativeSave()) {
      reason = "this profile's randomized save already has native saves; New Game would replace it";
      return false;
    }
    return true;  // never saved natively: the same lineage starts over
  }
  if (!g.server_ready || !g.server) {
    reason = "not connected to a validated Archipelago slot when the New Game started";
    return false;
  }
  const auto xuid = ProfileXuid();
  if (!xuid) { reason = "could not identify a single signed-in profile directory"; return false; }
  auto contract = *g.server;
  contract.binding.profile = *xuid;
  contract.binding.save = NewSaveUuid();
  fs::create_directories(g.sidecar.parent_path());
  g.session = std::make_unique<SessionState>(g.sidecar, contract);
  g.stored = contract;
  REXLOG_INFO("ap: New Game bound to seed {} slot {} as save {}", contract.binding.seed,
              contract.binding.slot, contract.binding.save);
  SendCommands(nlohmann::json::array({{{"cmd", "Sync"}}}));  // full inventory for the new binding
  return true;
}

void Decide(uint8_t* base) {
  bool bound = false;
  std::string reason;
  try {
    if (g.load_seen) {
      bound = g.load_bound;
      reason = g.load_reason;
    } else {
      bound = BindNewGame(reason);
    }
    if (bound) {
      std::string error;
      if (!VerifyCatalog(base, error)) return Halt("catalog does not match the loaded world: " + error);
      REXLOG_INFO("ap: verified {} catalog orbs against the loaded world", g.catalog->orbs.size());
      if (g.load_seen) {
        RecoverNativeChecks(base);
        if (!g.catalog->bosses.empty())
          g.session->RecordSave(ActiveBinding(), g.load_hash, NativeDead(base), NativeMaximumSkills(base));
      }
    }
  } catch (const std::exception& e) {
    return Halt(std::string("binding failed: ") + e.what());
  }
  g.mode = bound ? Mode::bound : Mode::foreign;
  if (bound) {
    REXLOG_INFO("ap: randomized save active ({}); gameplay XP disabled, {} pending check(s)",
                g.load_seen ? "native load" : "new game", g.session->PendingChecks().size());
    SendPendingChecks();
    SendGoal();
  } else {
    REXLOG_WARN("ap: this save is not the profile's randomized save ({}); vanilla rules apply", reason);
  }
}

void PollSaveWrite() {
  if (!g.watch_save || ++g.frame % 15) return;
  std::error_code ec;
  const auto path = SavePath(ActiveBinding().profile);
  const auto stamp = fs::last_write_time(path, ec);
  const auto size = ec ? 0 : fs::file_size(path, ec);
  if (ec || (stamp == g.watch_stamp && size == g.watch_size && g.watch_stable == 0)) return;
  if (stamp != g.watch_stamp || size != g.watch_size) {
    g.watch_stamp = stamp;
    g.watch_size = size;
    g.watch_stable = 1;
    return;
  }
  // Unchanged for one more poll: the native write is complete.
  const auto hash = HashFile(path);
  if (!hash) return;
  try {
    if (Fault("fail_record_save")) throw std::runtime_error("injected sidecar write failure");
    g.session->RecordSave(ActiveBinding(), *hash, g.watch_dead, g.watch_maximum_skills);
    REXLOG_INFO("ap: native save recorded in lineage ({} bytes, sha256 {})", size, hash->substr(0, 16));
    g.watch_save = false;
    g.record_failing = false;
    SendGoal();
  } catch (const std::exception& e) {
    // The native save already exists: keep retrying (every 15 frames) so it
    // joins the lineage as soon as the sidecar is writable again.
    if (!g.record_failing)
      REXLOG_ERROR("ap: could not record native save: {}; retrying", e.what());
    g.record_failing = true;
  }
}

void Reconcile(PPCContext& ctx, uint8_t* base) {
  const auto now = Clock::now();
  const auto updates = CharacterUpdateCount();
  if (updates != g.last_updates) {
    if (!g.steady && g.steady_since == Clock::time_point{}) g.steady_since = now;
    g.last_updates = updates;
  } else {
    g.steady = false;
    g.steady_since = {};
  }
  if (!g.steady && g.steady_since != Clock::time_point{} && now - g.steady_since > std::chrono::seconds(1))
    g.steady = true;
  if (g.mode == Mode::transition && g.steady && g.halt.empty()) Decide(base);
  if (!SaveIsBound() || !g.halt.empty()) return;
  SendGoal();

  if (g.retry_observe.empty()) {
    g.last_observe_retry = now;
  } else if (now - g.last_observe_retry > std::chrono::seconds(2)) {
    g.last_observe_retry = now;
    auto retry = std::move(g.retry_observe);
    g.retry_observe.clear();
    for (auto id : retry) Observe(id);
  }
  if (g.server_ready && now - g.last_check_send > std::chrono::seconds(10)) SendPendingChecks();

  const uint32_t skills = PrimarySkills(base);
  if (!skills || now - g.last_grant_try < std::chrono::milliseconds(500)) return;
  g.last_grant_try = now;
  ReconcileWeapons(base);
  if (!g.steady) return; // Skill setter effects need advancing character updates.
  for (int index = 0; index < 5; ++index) {
    const int native = REX_LOAD_U8(skills + 17 + index);
    if (native < 1 || native > 5) continue;
    std::optional<int> target;
    try {
      target = g.session->SkillTarget(ActiveBinding(), index, native, true);
    } catch (const std::exception& e) {
      return Halt(std::string(kSkillNames[index]) + " target: " + e.what());
    }
    if (!target) continue;
    auto grant = ctx;
    grant.r3.u64 = skills;
    grant.r4.u64 = static_cast<uint32_t>(index);
    grant.r5.u64 = static_cast<uint32_t>(*target);
    grant.r6.u64 = 1;
    __imp__sub_82543DB0(grant, base);
    const int after = REX_LOAD_U8(skills + 17 + index);
    if (after == *target)
      REXLOG_INFO("ap: {} {} -> {} (inventory {} item(s))", kSkillNames[index], native, after, g.session->Inventory().size());
    else
      REXLOG_ERROR("ap: {} setter left level {} (target {}); will retry", kSkillNames[index], after, *target);
  }
}
}  // namespace

// --- public entry points ---------------------------------------------------

void Initialize(const fs::path& user_data_root) {
  std::lock_guard lock(g_mutex);
  g.root = user_data_root;
  g.sidecar = user_data_root / "archipelago" / "session.toml";
  try {
    g.catalog = &EmbeddedCatalog();
    g.stored = SessionState::ReadContract(g.sidecar);
    if (g.stored) {
      ValidateStoredContract(*g.catalog, *g.stored);
      g.session = std::make_unique<SessionState>(g.sidecar, *g.stored);
    }
  } catch (const std::exception& e) {
    g.enabled = true;
    Halt(std::string("cannot open ") + g.sidecar.string() + ": " + e.what());
    return;
  }
  const auto server = REXCVAR_GET(ap_server);
  g.enabled = g.stored.has_value() || !server.empty();
  if (!g.enabled) return;
  REXLOG_INFO("ap: catalog {}; profile session {}; server {}", g.catalog->version,
              g.stored ? "seed " + g.stored->binding.seed + " save " + g.stored->binding.save : "unbound",
              server.empty() ? "none (offline)" : server);
  if (!server.empty()) g.client.Start(server);
}

void Shutdown() {
  g.client.Stop();
}

void OnFrame(PPCContext& ctx, uint8_t* base) {
  {
    std::lock_guard lock(g_mutex);
    if (g.enabled) {
      if (g.halt.empty()) PumpNetwork();
      if (g.halt.empty() && SaveIsBound()) PollSaveWrite();
      Reconcile(ctx, base);
    }
  }
  // Diagnostics can re-enter native observer hooks; never hold the coordinator
  // lock across these calls. Disabled unless --ap_probe plus an explicit ID.
  ProbeNativeAdapters(ctx, base);
}

bool SuppressXP(uint32_t skillset, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  if (!g.enabled || !SaveIsBound()) return false;
  return skillset == PrimarySkills(base);
}

void OnSupplyCaptured(uint32_t player, uint32_t index, uint32_t caller, bool changed, uint8_t* base) {
  if (!NaturalSupplyCapture(player, index, caller, changed)) return;
  std::lock_guard lock(g_mutex);
  if (!g.enabled || !SaveIsBound() || !g.halt.empty() || !g.catalog->supplies.contains(index)) return;
  if (!VerifySupplies(base)) return Halt("supply capture table mismatch");
  const auto& supply = g.catalog->supplies.at(index);
  const uint32_t manager = REX_LOAD_U32(0x82DE2678);
  if (REX_LOAD_U8(manager + 2616 + index) != 1) return;
  if (g.session->contract().locations.contains(supply.id)) {
    REXLOG_INFO("ap: supply captured index={} player={} caller={:08X}", index, player, caller);
    Observe(supply.id);
  }
}

bool HandleWeaponDeposit(PPCContext& ctx, uint8_t* base) {
  if (!NaturalDeposit(ctx.r4.u32, REX_LOAD_U32(kPlayerGlobal), static_cast<uint32_t>(ctx.lr))) return false;
  uint32_t begin = 0;
  {
    std::lock_guard lock(g_mutex);
    if (!g.enabled || !SaveIsBound() || !g.halt.empty() || g.catalog->weapons.empty() || g_deposit) return false;
    if (!PrimarySkills(base) || !(begin = Armory(base))) {
      Halt("deposit player/armory layout mismatch"); return false;
    }
  }
  DepositPass pass{base, begin};
  for (int i = 0; i < 19; ++i) {
    pass.before[i] = REX_LOAD_U8(begin + 112 * i + 9);
    // Only within this synchronous native scan: its already-available branch
    // avoids unlocking and avoids adding a misleading vanilla unlock popup.
    REX_STORE_U8(begin + 112 * i + 9, 1);
  }
  g_deposit = &pass;
  __imp__sub_82642760(ctx, base);
  pass.Restore(); // also guaranteed on exceptions by the scope destructor
  std::lock_guard lock(g_mutex);
  if (!SaveIsBound() || !g.halt.empty()) return true;
  if (Armory(base) != begin) { Halt("armory changed during deposit"); return true; }
  for (int id : pass.carried) {
    const auto& w = g.catalog->weapons.at(id);
    REXLOG_INFO("ap: carried weapon deposited native_id={} caller=826429A0", id);
    if (g.session->contract().locations.contains(w.id)) Observe(w.id);
  }
  ReconcileWeapons(base);
  return true;
}
}  // namespace crackdown::ap

using namespace crackdown::ap;

REX_HOOK_RAW(sub_82644A50) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82644A50(ctx, base);
  if (!g_deposit || !DepositResolver(caller)) return;
  const uint32_t entry = ctx.r3.u32, begin = g_deposit->begin;
  if (entry < begin || entry >= begin + 19 * 112 || (entry - begin) % 112) return;
  const uint32_t id = REX_LOAD_U32(entry + 12);
  if (id == 1 + (entry - begin) / 112) g_deposit->carried.insert(static_cast<int>(id));
}

REX_HOOK_RAW(sub_823CBA98) {
  const uint32_t boss = ctx.r3.u32;
  // Gang weakening, world state, achievements, effects and native dead flag
  // all remain owned by the original callback. Inspect only after it returns.
  __imp__sub_823CBA98(ctx, base);
  std::lock_guard lock(g_mutex);
  if (!g.enabled || !SaveIsBound() || !g.halt.empty() || g.catalog->bosses.empty()) return;
  const auto bosses = BossTable(base);
  if (!bosses) return Halt("boss defeat layout/catalog mismatch");
  for (const auto& [id, address] : *bosses) {
    if (address != boss) continue;
    if (REX_LOAD_U8(boss + 148) == 1) {
      const auto location = g.catalog->bosses.at(id).id;
      REXLOG_INFO("ap: native boss defeated native_id={} dead=1 callback=823CBA98", id);
      if (g.session->contract().locations.contains(location)) Observe(location);
    }
    return;
  }
  REXLOG_WARN("ap: boss callback object {:08X} outside supported table", boss);
}

REX_HOOK_RAW(sub_8254E1A0) {
  bool primary = false;
  {
    std::lock_guard lock(g_mutex);
    primary = ctx.r4.u32 == 0 && g.enabled && SaveIsBound() && g.halt.empty();
    if (primary) {
      g.native_saving = true;
      g.serialized_bosses.clear();
      g.serialized_dead.clear();
      g.watch_dead.clear();
    }
  }
  __imp__sub_8254E1A0(ctx, base); // every native allocation, serializer and write retained
  if (!primary) return;
  std::lock_guard lock(g_mutex);
  g.native_saving = false;
  if (g.watch_save && g.serialized_bosses.size() == 21) {
    g.watch_dead = g.serialized_dead;
    REXLOG_INFO("ap: native save serialized {} bosses ({} dead), kingpins {}/{}/{}",
        g.serialized_bosses.size(), g.watch_dead.size(), g.watch_dead.contains(73402003),
        g.watch_dead.contains(73402011), g.watch_dead.contains(73402017));
  }
}

REX_HOOK_RAW(sub_823CC1A8) {
  // cBossInfo's save interface is embedded at +28 (vtable 820A515C).
  // Its serializer writes bytes +116..120 = boss +144..148. Capture precisely
  // the +120 value it writes, only in the bound primary save and real write pass.
  const uint32_t object = ctx.r3.u32, stream = ctx.r4.u32;
  std::optional<int64_t> location;
  bool dead = false;
  {
    std::lock_guard lock(g_mutex);
    if (g.native_saving && g.enabled && SaveIsBound() && g.halt.empty() && object >= 28 &&
        stream && REX_LOAD_U8(stream + 76) != 0) {
      const auto bosses = BossTable(base);
      if (bosses) for (const auto& [id, boss] : *bosses) if (boss + 28 == object) {
        location = g.catalog->bosses.at(id).id;
        dead = REX_LOAD_U8(object + 120) == 1;
        break;
      }
    }
  }
  __imp__sub_823CC1A8(ctx, base);
  if (!location) return;
  std::lock_guard lock(g_mutex);
  g.serialized_bosses.insert(*location);
  if (dead) g.serialized_dead.insert(*location);
  else g.serialized_dead.erase(*location);
}

REX_HOOK_RAW(sub_824EEEE8) {
  {
    std::lock_guard lock(g_mutex);
    if (g.enabled) {
      g.mode = Mode::transition;
      g.load_seen = false;
      g.load_hash.clear();
      g.watch_save = false;
      g.steady = false;
      g.steady_since = {};
    }
  }
  __imp__sub_824EEEE8(ctx, base);
}

REX_HOOK_RAW(sub_824EF158) {
  const bool primary_load = ctx.r5.u32 == 0 && static_cast<uint32_t>(ctx.lr) == kNativeLoadLR;
  __imp__sub_824EF158(ctx, base);
  if (!primary_load) return;
  std::lock_guard lock(g_mutex);
  if (!g.enabled) return;
  g.load_seen = true;
  g.load_bound = false;
  g.load_reason = "profile has no randomized session";
  if (!g.session || !g.halt.empty()) return;
  try {
    const auto path = SavePath(g.session->binding().profile);
    const auto hash = HashFile(path);
    if (!hash) { g.load_reason = "could not read the native save file"; return; }
    g.load_bound = g.session->AcceptLoad(g.session->binding(), *hash, WriteTime(path));
    if (g.load_bound) g.load_hash = *hash;
    g.load_reason = g.load_bound ? "" : "native save " + hash->substr(0, 16) + " is not in the bound lineage";
    REXLOG_INFO("ap: native load sha256 {} -> {}", hash->substr(0, 16), g.load_bound ? "bound" : "foreign");
  } catch (const std::exception& e) {
    Halt(std::string("native load check failed: ") + e.what());
  }
}

REX_HOOK_RAW(sub_824EF1E0) {
  const bool write = REX_LOAD_U8(ctx.r4.u32 + 76) != 0 && ctx.r5.u32 == 0 &&
                     static_cast<uint32_t>(ctx.lr) == kNativeSaveLR;
  if (write) {
    std::lock_guard lock(g_mutex);
    if (g.enabled && SaveIsBound() && g.halt.empty()) {
      // Boss goal evidence comes from each boss serializer's actual write pass,
      // assembled at the end of 8254E1A0. Live memory at this orb serializer is
      // insufficient: boss chunks may already have been written.
      g.watch_maximum_skills = false;
      g.watch_maximum_skills = NativeMaximumSkills(base);
      // Durable before the game writes the file: a crash between the write and
      // RecordSave still lets that one save load as bound (if written within
      // SessionState::kPendingWindow). This hook sits inside the native save and
      // cannot cancel it, so if the marker can't be written the save is watched
      // anyway and recorded by PollSaveWrite once the sidecar is writable.
      try {
        if (Fault("fail_begin_save")) throw std::runtime_error("injected sidecar write failure");
        g.session->BeginSave(g.session->binding(), Now());
      } catch (const std::exception& e) {
        REXLOG_ERROR("ap: could not mark native save pending: {}; recording it after the write",
                     e.what());
      }
      std::error_code ec;
      const auto path = SavePath(g.session->binding().profile);
      g.watch_stamp = fs::last_write_time(path, ec);
      g.watch_size = ec ? 0 : fs::file_size(path, ec);
      g.watch_stable = 0;
      g.watch_save = true;
    }
  }
  __imp__sub_824EF1E0(ctx, base);
}

namespace {
void PickupSelected(uint32_t pickup, uint32_t player, uint8_t* base) {
  if (!pickup || player != 0) return;
  std::lock_guard lock(g_mutex);
  if (!g.enabled || !SaveIsBound() || !g.halt.empty()) return;
  const uint32_t manager = REX_LOAD_U32(kPickupManagerGlobal);
  const uint32_t count = manager ? REX_LOAD_U32(manager + 4012) : 0;
  for (uint32_t i = 0; i < count && i < 1000; ++i) {
    if (REX_LOAD_U32(manager + 12 + 4 * i) != pickup) continue;
    const auto* orb = g.catalog->ByOrdinal(i);
    if (!orb || REX_LOAD_U32(pickup + 264) != orb->type) {
      REXLOG_ERROR("ap: collected pickup ordinal {} does not match the catalog", i);
      return;
    }
    REXLOG_INFO("ap: orb collected ordinal {} type {}", i, orb->type);
    if (g.session->contract().locations.contains(orb->id)) Observe(orb->id);
    return;
  }
  REXLOG_ERROR("ap: collected pickup {:08X} is not in the pickup manager", pickup);
}
}  // namespace

REX_HOOK_RAW(sub_824EF6F8) {
  const uint32_t player = ctx.r5.u32;
  __imp__sub_824EF6F8(ctx, base);
  PickupSelected(ctx.r3.u32, player, base);
}

REX_HOOK_RAW(sub_824EF588) {
  const uint32_t player = ctx.r5.u32;
  __imp__sub_824EF588(ctx, base);
  PickupSelected(ctx.r3.u32, player, base);
}

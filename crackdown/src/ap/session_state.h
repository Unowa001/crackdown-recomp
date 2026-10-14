// Host-only AP state. No sockets, guest memory, or consumable delivery.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "skill_progression.h"
#include "weapon_progression.h"

namespace crackdown::ap {
struct Binding {
  std::string game, seed, profile, save, catalog;
  int schema = 1, team = 0, slot = 0;
  bool operator==(const Binding&) const = default;
};
// Supplied by the validated developer-world catalog; no public IDs assigned here.
struct Contract {
  Binding binding;
  int64_t agility_item = 0;
  std::set<int64_t> locations;
  int starting_agility = 1, maximum_agility = 5;
  // Empty for the original M1 contract; M2 declares all five native indices.
  std::map<int, SkillProgression> skills;
  std::map<int, WeaponProgression> weapons; // native ID -> permanent receipt
  int64_t filler_item = 0; // Agency Commendation: permanent no-op
  std::string goal;
  std::set<int64_t> kingpin_locations;
  bool operator==(const Contract&) const = default;
};
struct NetworkItem {
  int64_t item = 0, location = 0;
  int player = 0, flags = 0;
  bool operator==(const NetworkItem&) const = default;
};
enum class Receipt { replaced, appended, resync_required };

// One coordinator owns this object. Network callbacks must queue copied packets.
// Mutations commit a complete sidecar before becoming visible to the caller.
class SessionState {
 public:
  SessionState(std::filesystem::path path, Contract contract);
  Receipt Receive(const Binding&, int64_t index, const std::vector<NetworkItem>&);
  bool Observe(const Binding&, int64_t location); // persist BEFORE sending a check
  // authoritative (Connected.checked_locations) replaces the set, so a check a
  // rolled-back server forgot is pending again; RoomUpdate only adds.
  void Acknowledge(const Binding&, const std::set<int64_t>& server_checked,
                   bool authoritative = false);
  std::vector<int64_t> PendingChecks() const;
  // Ready must be proven by the game adapter (active bound save + valid player).
  // Setter effects are absolute; call again after every native save load.
  std::optional<int> AgilityTarget(const Binding& active_save, int native_level,
                                  bool gameplay_ready) const;
  std::optional<int> SkillTarget(const Binding& active_save, int skill_index,
                                int native_level, bool gameplay_ready) const;
  bool NeedsResync() const { return resync_; }
  std::optional<bool> WeaponTarget(const Binding&, int native_id, bool ready) const;
  // Native save lineage. The adapter marks a native save write as pending
  // before the game writes it, then records the written file's SHA-256. A load
  // is bound only when its hash belongs to this lineage. A pending write that a
  // crash interrupted before recording is accepted once, and only when the file
  // was last written within kPendingWindow seconds after BeginSave (times are
  // Unix seconds; a copied-in foreign save keeps its older timestamp).
  static constexpr int64_t kPendingWindow = 300;
  void BeginSave(const Binding&, int64_t now);
  void RecordSave(const Binding&, const std::string& sha256,
                  const std::set<int64_t>& native_dead = {}, bool maximum_skills = false);
  // Monotonic native-save evidence. The bridge resends CLIENT_GOAL once per
  // validated connection because StatusUpdate has no delivery acknowledgement.
  bool GoalComplete() const { return !state_.goal_save.empty(); }
  bool AcceptLoad(const Binding&, const std::string& sha256, std::optional<int64_t> written);
  bool HasNativeSave() const { return state_.save_pending || !state_.saves.empty(); }
  // Reads the binding/catalog contract of an existing sidecar so an offline
  // start can reopen it; nullopt when absent. Corrupt files throw.
  static std::optional<Contract> ReadContract(const std::filesystem::path&);
  const std::vector<NetworkItem>& Inventory() const { return state_.inventory; }
  const Contract& contract() const { return contract_; }
  const std::set<int64_t>& Observed() const { return state_.observed; }
  const Binding& binding() const { return contract_.binding; }
 private:
  struct State {
    std::vector<NetworkItem> inventory;
    std::set<int64_t> observed, acknowledged;
    bool inventory_known = false;
    std::set<std::string> saves;
    bool save_pending = false;
    int64_t save_pending_at = 0;  // Unix seconds of the latest BeginSave
    std::string goal_save; // lineage hash witnessing the native goal; never receipts/check acks
  };
  void Verify(const Binding&) const;
  void Commit(State next);
  std::string Encode(const State&) const;
  State Decode(const std::string&) const;
  std::filesystem::path path_;
  Contract contract_;
  State state_;
  bool resync_ = false;
};
} // namespace crackdown::ap

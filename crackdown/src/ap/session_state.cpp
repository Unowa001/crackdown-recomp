#include "session_state.h"
#include "../../common/toml_edit.h"
#include <algorithm>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

namespace crackdown::ap {
namespace {
constexpr size_t kMaxItems = 100000, kMaxLocations = 10000, kMaxSaves = 100000,
                 kMaxBytes = 16 * 1024 * 1024;
constexpr int64_t kMaxId = (int64_t{1} << 53) - 1;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool Id(int64_t id) { return id >= -kMaxId && id <= kMaxId; }
template<class T> T Value(const toml::table& t, const char* key) {
  auto value = t[key].value<T>();
  Require(value.has_value(), "Invalid AP sidecar field");
  return *value;
}
void ValidateItem(const NetworkItem& item, const Contract& contract) {
  bool supported = contract.skills.empty() && item.item == contract.agility_item;
  for (const auto& [index, skill] : contract.skills) supported |= item.item == skill.item;
  for (const auto& [id, weapon] : contract.weapons) supported |= item.item == weapon.item;
  supported |= contract.filler_item != 0 && item.item == contract.filler_item;
  Require(supported && Id(item.location) && item.player >= 0 &&
          item.flags >= 0 && item.flags <= 7, "Unsupported AP item");
}
toml::array Ids(const std::set<int64_t>& ids) {
  toml::array out;
  for (auto id : ids) out.push_back(id);
  return out;
}
bool Sha256(const std::string& h) {
  return h.size() == 64 && std::all_of(h.begin(), h.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
std::set<int64_t> ReadIds(const toml::table& t, const char* key) {
  auto array = t[key].as_array();
  Require(array && array->size() <= kMaxLocations, "Invalid AP location list");
  std::set<int64_t> ids;
  for (const auto& node : *array) {
    auto id = node.value<int64_t>();
    Require(id && Id(*id) && ids.insert(*id).second, "Invalid/duplicate AP location");
  }
  return ids;
}
Contract ParseContract(const toml::table& t) {
  Require(Value<int>(t, "format") == 2, "Unsupported AP sidecar version");
  Contract c;
  c.binding = {Value<std::string>(t, "game"), Value<std::string>(t, "seed"),
               Value<std::string>(t, "profile"), Value<std::string>(t, "save"),
               Value<std::string>(t, "catalog"), Value<int>(t, "schema"),
               Value<int>(t, "team"), Value<int>(t, "slot")};
  c.agility_item = Value<int64_t>(t, "agility_item");
  c.starting_agility = Value<int>(t, "starting_agility");
  c.maximum_agility = Value<int>(t, "maximum_agility");
  if (t.contains("skills")) {
    auto skills = t["skills"].as_array();
    Require(skills && skills->size() == 5, "Invalid AP skill contract");
    for (const auto& node : *skills) {
      auto skill = node.as_table();
      Require(skill != nullptr, "Invalid AP skill contract");
      Require(c.skills.emplace(Value<int>(*skill, "index"), SkillProgression{
          Value<int64_t>(*skill, "item"), Value<int>(*skill, "starting"),
          Value<int>(*skill, "maximum")}).second, "Duplicate AP skill index");
    }
  }
  c.locations = ReadIds(t, "locations");
  if (t.contains("weapons")) {
    auto weapons = t["weapons"].as_array();
    Require(weapons && weapons->size() == 19, "Invalid AP weapon contract");
    for (const auto& node : *weapons) {
      auto w = node.as_table();
      Require(w && c.weapons.emplace(Value<int>(*w, "native_id"), WeaponProgression{
          Value<int64_t>(*w, "item"), Value<bool>(*w, "starting")}).second,
          "Invalid/duplicate AP weapon contract");
    }
    c.filler_item = Value<int64_t>(t, "filler_item");
    c.goal = Value<std::string>(t, "goal");
    c.kingpin_locations = ReadIds(t, "kingpin_locations");
  }
  return c;
}
std::string ReadFile(const std::filesystem::path& path) {
  Require(std::filesystem::file_size(path) <= kMaxBytes, "AP sidecar is too large");
  std::ifstream in(path, std::ios::binary);
  Require(bool(in), "Cannot read AP sidecar");
  std::string text{std::istreambuf_iterator<char>(in), {}};
  Require(!in.bad(), "Cannot read AP sidecar");
  return text;
}
}

SessionState::SessionState(std::filesystem::path path, Contract contract)
    : path_(std::move(path)), contract_(std::move(contract)) {
  const auto& b = contract_.binding;
  Require(b.game == "Crackdown" && b.schema == 1 && !b.seed.empty() &&
          !b.profile.empty() && !b.save.empty() && !b.catalog.empty() &&
          b.team >= 0 && b.slot > 0, "Invalid AP binding");
  for (const auto* text : {&b.seed, &b.profile, &b.save, &b.catalog})
    Require(text->size() <= 1024, "AP identity is too long");
  Require(Id(contract_.agility_item) && contract_.starting_agility >= 1 &&
          contract_.maximum_agility <= 5 &&
          contract_.starting_agility <= contract_.maximum_agility &&
          !contract_.locations.empty() && contract_.locations.size() <= kMaxLocations,
          "Invalid AP catalog");
  for (auto id : contract_.locations) Require(Id(id), "Invalid catalog location");
  if (!contract_.skills.empty()) {
    Require(contract_.skills.size() == 5 && contract_.skills.contains(0), "Invalid AP skill contract");
    Require(contract_.skills.at(0) == SkillProgression{contract_.agility_item,
        contract_.starting_agility, contract_.maximum_agility}, "Agility contract differs from skills");
    std::set<int64_t> item_ids;
    for (const auto& [index, skill] : contract_.skills)
      Require(index >= 0 && index < 5 && Id(skill.item) &&
          item_ids.insert(skill.item).second && !contract_.locations.contains(skill.item) &&
          skill.starting >= 1 && skill.maximum <= 5 && skill.starting <= skill.maximum,
          "Invalid AP skill contract");
  }
  if (!contract_.weapons.empty()) {
    Require(contract_.weapons.size() == 19 && contract_.skills.size() == 5 &&
        (contract_.goal == "kingpins" || contract_.goal == "max_skills") &&
        contract_.kingpin_locations.size() == 3, "Invalid M2 contract");
    std::set<int64_t> items;
    for (const auto& [index, skill] : contract_.skills) items.insert(skill.item);
    for (const auto& [id, w] : contract_.weapons)
      Require(id >= 1 && id <= 19 && Id(w.item) && items.insert(w.item).second &&
          !contract_.locations.contains(w.item), "Invalid AP weapon contract");
    Require(contract_.filler_item != 0 && Id(contract_.filler_item) &&
        items.insert(contract_.filler_item).second &&
        !contract_.locations.contains(contract_.filler_item), "Invalid AP filler contract");
    for (auto id : contract_.kingpin_locations)
      Require(contract_.locations.contains(id), "Invalid AP kingpin contract");
  } else {
    Require(contract_.filler_item == 0 && contract_.goal.empty() &&
        contract_.kingpin_locations.empty(), "Incomplete M2 contract");
  }
  if (std::filesystem::exists(path_)) {
    state_ = Decode(ReadFile(path_)); // Corrupt/mismatched state is never silently reset.
  } else {
    Commit({}); // Establish durable binding before any grants or observations.
  }
}
void SessionState::Verify(const Binding& binding) const {
  Require(binding == contract_.binding, "AP seed/team/slot/profile/save/catalog mismatch");
}
void SessionState::Commit(State next) {
  auto text = Encode(next);
  Require(text.size() <= kMaxBytes, "AP sidecar is too large");
  config::ConfigTemp temp(path_);
  temp.replace(path_, text);
  state_ = std::move(next);
}
Receipt SessionState::Receive(const Binding& binding, int64_t index,
                              const std::vector<NetworkItem>& items) {
  Verify(binding);
  Require(index >= 0 && items.size() <= kMaxItems, "Invalid AP receipt index/size");
  for (const auto& item : items) ValidateItem(item, contract_);
  // Strict protocol resync: even duplicate/overlapping nonzero batches request
  // Sync + LocationChecks. No partial gap or conflict is added to inventory.
  if (index != 0 && (resync_ || !state_.inventory_known ||
                    static_cast<uint64_t>(index) != state_.inventory.size())) {
    resync_ = true;
    return Receipt::resync_required;
  }
  auto next = state_;
  if (index == 0) next.inventory = items;
  else {
    Require(items.size() <= kMaxItems - next.inventory.size(), "AP inventory is too large");
    next.inventory.insert(next.inventory.end(), items.begin(), items.end());
  }
  next.inventory_known = true;
  Commit(std::move(next));
  resync_ = false;
  return index == 0 ? Receipt::replaced : Receipt::appended;
}
bool SessionState::Observe(const Binding& binding, int64_t location) {
  Verify(binding);
  Require(contract_.locations.contains(location), "Location outside AP catalog");
  if (state_.observed.contains(location)) return false;
  auto next = state_;
  next.observed.insert(location);
  Commit(std::move(next));
  return true;
}
void SessionState::Acknowledge(const Binding& binding, const std::set<int64_t>& checked,
                               bool authoritative) {
  Verify(binding);
  for (auto id : checked) Require(contract_.locations.contains(id), "Server location outside AP catalog");
  auto next = state_;
  if (authoritative) next.acknowledged = checked;
  else next.acknowledged.insert(checked.begin(), checked.end());
  if (next.acknowledged != state_.acknowledged) Commit(std::move(next));
}
std::vector<int64_t> SessionState::PendingChecks() const {
  std::vector<int64_t> pending;
  std::set_difference(state_.observed.begin(), state_.observed.end(),
                      state_.acknowledged.begin(), state_.acknowledged.end(),
                      std::back_inserter(pending));
  return pending;
}
std::optional<Contract> SessionState::ReadContract(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return std::nullopt;
  return ParseContract(toml::parse(ReadFile(path)));
}
void SessionState::BeginSave(const Binding& binding, int64_t now) {
  Verify(binding);
  Require(now > 0, "Invalid save time");
  if (state_.save_pending && state_.save_pending_at == now) return;
  auto next = state_;
  next.save_pending = true;
  next.save_pending_at = now;
  Commit(std::move(next));
}
void SessionState::RecordSave(const Binding& binding, const std::string& sha256,
                              const std::set<int64_t>& native_dead, bool maximum_skills) {
  Verify(binding);
  Require(Sha256(sha256), "Invalid native save hash");
  Require(state_.saves.size() < kMaxSaves || state_.saves.contains(sha256),
          "Too many native saves in AP lineage");
  const bool goal = (contract_.goal == "kingpins" && std::includes(native_dead.begin(), native_dead.end(),
      contract_.kingpin_locations.begin(), contract_.kingpin_locations.end())) ||
      (contract_.goal == "max_skills" && maximum_skills);
  if (!state_.save_pending && state_.saves.contains(sha256) && (!goal || GoalComplete())) return;
  auto next = state_;
  next.saves.insert(sha256);
  next.save_pending = false;
  if (goal && next.goal_save.empty()) next.goal_save = sha256;
  Commit(std::move(next));
}
bool SessionState::AcceptLoad(const Binding& binding, const std::string& sha256,
                              std::optional<int64_t> written) {
  Verify(binding);
  Require(Sha256(sha256), "Invalid native save hash");
  if (state_.saves.contains(sha256)) return true;
  if (!state_.save_pending || !state_.save_pending_at || !written) return false;
  // Small allowance for filesystem timestamp rounding before the marker.
  if (*written < state_.save_pending_at - 2 || *written > state_.save_pending_at + kPendingWindow)
    return false;
  RecordSave(binding, sha256);
  return true;
}
std::optional<int> SessionState::AgilityTarget(const Binding& active, int native,
                                             bool ready) const {
  return SkillTarget(active, 0, native, ready);
}
std::optional<int> SessionState::SkillTarget(const Binding& active, int index, int native,
                                            bool ready) const {
  Verify(active);
  Require(index >= 0 && index < 5, "Invalid native skill index");
  if (contract_.skills.empty() && index != 0) return {};
  const auto skill = contract_.skills.empty()
      ? SkillProgression{contract_.agility_item, contract_.starting_agility, contract_.maximum_agility}
      : contract_.skills.at(index);
  if (!ready || resync_ || !state_.inventory_known) return {};
  Require(native >= 1 && native <= 5, "Invalid native skill level");
  const auto count = std::count_if(state_.inventory.begin(), state_.inventory.end(),
      [&](const auto& item) { return item.item == skill.item; });
  int target = static_cast<int>(std::min<int64_t>(skill.maximum, skill.starting + count));
  return native == target ? std::nullopt : std::optional<int>(target);
}
std::optional<bool> SessionState::WeaponTarget(const Binding& active, int id, bool ready) const {
  Verify(active);
  Require(contract_.weapons.contains(id), "Invalid native weapon ID");
  if (!ready || resync_ || !state_.inventory_known) return {};
  const auto& weapon = contract_.weapons.at(id);
  return weapon.starting || std::any_of(state_.inventory.begin(), state_.inventory.end(),
      [&](const auto& i) { return i.item == weapon.item; });
}
std::string SessionState::Encode(const State& s) const {
  const auto& b = contract_.binding;
  toml::table t{{"format", 2}, {"game", b.game}, {"schema", b.schema},
                {"seed", b.seed}, {"profile", b.profile}, {"save", b.save},
                {"catalog", b.catalog}, {"team", b.team}, {"slot", b.slot},
                {"agility_item", contract_.agility_item},
                {"starting_agility", contract_.starting_agility},
                {"maximum_agility", contract_.maximum_agility},
                {"locations", Ids(contract_.locations)},
                {"inventory_known", s.inventory_known},
                {"observed", Ids(s.observed)}, {"acknowledged", Ids(s.acknowledged)},
                {"save_pending", s.save_pending}, {"save_pending_at", s.save_pending_at},
                {"goal_save", s.goal_save}};
  toml::array saves;
  if (!contract_.weapons.empty()) {
    toml::array weapons;
    for (const auto& [id, w] : contract_.weapons)
      weapons.push_back(toml::table{{"native_id", id}, {"item", w.item}, {"starting", w.starting}});
    t.insert("weapons", std::move(weapons));
    t.insert("filler_item", contract_.filler_item);
    t.insert("goal", contract_.goal);
    t.insert("kingpin_locations", Ids(contract_.kingpin_locations));
  }
  if (!contract_.skills.empty()) {
    toml::array skills;
    for (const auto& [index, skill] : contract_.skills)
      skills.push_back(toml::table{{"index", index}, {"item", skill.item},
                                  {"starting", skill.starting}, {"maximum", skill.maximum}});
    t.insert("skills", std::move(skills));
  }
  for (const auto& hash : s.saves) saves.push_back(hash);
  t.insert("saves", std::move(saves));
  toml::array items;
  for (const auto& i : s.inventory)
    items.push_back(toml::table{{"item", i.item}, {"location", i.location},
                               {"player", i.player}, {"flags", i.flags}});
  t.insert("inventory", std::move(items));
  std::ostringstream out;
  out << toml::toml_formatter{t};
  return out.str();
}
SessionState::State SessionState::Decode(const std::string& text) const {
  auto t = toml::parse(text);
  const auto stored = ParseContract(t);
  Verify(stored.binding);
  Require(stored == contract_, "AP catalog contract mismatch");
  State s;
  s.save_pending = Value<bool>(t, "save_pending");
  // Absent in sidecars from before the timestamp: such a pending write is never accepted.
  s.save_pending_at = t["save_pending_at"].value<int64_t>().value_or(0);
  auto saves = t["saves"].as_array();
  Require(saves && saves->size() <= kMaxSaves, "Invalid persisted native save list");
  for (const auto& node : *saves) {
    auto hash = node.value<std::string>();
    Require(hash && Sha256(*hash) && s.saves.insert(*hash).second, "Invalid persisted native save");
  }
  s.inventory_known = Value<bool>(t, "inventory_known");
  s.goal_save = t["goal_save"].value<std::string>().value_or("");
  Require(s.goal_save.empty() || (!contract_.goal.empty() && Sha256(s.goal_save) &&
      s.saves.contains(s.goal_save)), "Invalid goal save witness");
  s.observed = ReadIds(t, "observed");
  s.acknowledged = ReadIds(t, "acknowledged");
  for (const auto* ids : {&s.observed, &s.acknowledged})
    for (auto id : *ids) Require(contract_.locations.contains(id), "Invalid persisted AP location");
  auto items = t["inventory"].as_array();
  Require(items && items->size() <= kMaxItems, "Invalid persisted AP inventory");
  for (const auto& node : *items) {
    auto item = node.as_table();
    Require(item != nullptr, "Invalid persisted AP item");
    NetworkItem i{Value<int64_t>(*item, "item"), Value<int64_t>(*item, "location"),
                  Value<int>(*item, "player"), Value<int>(*item, "flags")};
    ValidateItem(i, contract_);
    s.inventory.push_back(i);
  }
  Require(s.inventory_known || s.inventory.empty(), "Uninitialized AP inventory has items");
  return s;
}
} // namespace crackdown::ap

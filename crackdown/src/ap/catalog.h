// Versioned AP catalog (crackdown/ap/catalog.json, embedded at build time).
// The same file is the ID source for the Python world.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include "skill_progression.h"
#include "session_state.h"

namespace crackdown::ap {
struct OrbEntry {
  int64_t id = 0;
  uint32_t ordinal = 0, type = 0;
  std::string name;
  std::array<float, 3> position{};
};
struct Catalog {
  std::string version, game;
  int64_t agility_item = 0;
  int starting_agility = 1, maximum_agility = 5;
  std::map<int, SkillProgression> skills;
  std::map<int64_t, OrbEntry> orbs;  // by location ID
  struct Supply { int64_t id; int index; bool starting; std::array<float, 3> position; };
  struct Weapon { int64_t id, item; int native_id, index; bool starting; std::string name; };
  struct Boss { int64_t id; int native_id; bool kingpin; };
  std::map<int, Supply> supplies;
  std::map<int, Weapon> weapons;
  std::map<int, Boss> bosses;
  int64_t filler_item = 0;
  std::set<int64_t> Locations(bool hidden = true) const;
  const OrbEntry* ByOrdinal(uint32_t ordinal) const;
};
Catalog ParseCatalog(std::string_view json);  // throws on invalid catalog
const Catalog& EmbeddedCatalog();
// Host-testable slot_data validation, shared by the live bridge.
Contract ParseSlotContract(const Catalog&, std::string_view slot_data,
                           const std::set<int64_t>& locations);
void ValidateStoredContract(const Catalog&, const Contract&); // throws; never migrates a binding
}  // namespace crackdown::ap

#include "catalog.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <algorithm>
#include <set>
#include <cmath>

#include "ap_catalog_data.h"  // generated: kCatalogJson / kCatalogJsonSize

namespace crackdown::ap {
namespace {
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
}  // namespace

const OrbEntry* Catalog::ByOrdinal(uint32_t ordinal) const {
  // IDs are location_base + ordinal, but look up through the table so a
  // future catalog cannot silently diverge from that formula.
  for (const auto& [id, orb] : orbs)
    if (orb.ordinal == ordinal) return &orb;
  return nullptr;
}

Catalog ParseCatalog(std::string_view text) {
  auto j = nlohmann::json::parse(text);
  Catalog c;
  c.version = j.at("version").get<std::string>();
  c.game = j.at("game").get<std::string>();
  c.agility_item = j.at("items").at("Progressive Agility").get<int64_t>();
  c.starting_agility = j.at("agility").at("starting").get<int>();
  c.maximum_agility = j.at("agility").at("maximum").get<int>();
  Require(c.game == "Crackdown" && !c.version.empty(), "Invalid AP catalog header");
  Require(c.starting_agility >= 1 && c.starting_agility <= c.maximum_agility &&
          c.maximum_agility <= 5, "Invalid AP catalog Agility bounds");
  if (j.contains("skills")) {
    Require(j.at("skills").is_array() && j.at("skills").size() == 5, "Invalid AP catalog skills");
    std::set<int64_t> items;
    for (const auto& s : j.at("skills")) {
      const int index = s.at("index").get<int>();
      SkillProgression skill{s.at("item").get<int64_t>(), s.at("starting").get<int>(),
                             s.at("maximum").get<int>()};
      Require(index >= 0 && index < 5 && skill.starting >= 1 && skill.maximum <= 5 &&
          skill.starting <= skill.maximum && items.insert(skill.item).second &&
          c.skills.emplace(index, skill).second, "Invalid AP catalog skill");
    }
    Require(c.skills.at(0) == SkillProgression{c.agility_item, c.starting_agility,
        c.maximum_agility}, "AP catalog Agility contract mismatch");
  }
  std::set<uint32_t> ordinals;
  for (const auto& o : j.at("orbs")) {
    OrbEntry e;
    e.id = o.at("id").get<int64_t>();
    e.ordinal = o.at("ordinal").get<uint32_t>();
    e.type = o.at("type").get<uint32_t>();
    e.name = o.at("name").get<std::string>();
    const auto& p = o.at("position");
    e.position = {p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>()};
    Require(e.ordinal < 800 && e.type <= 4 && ordinals.insert(e.ordinal).second &&
        std::all_of(e.position.begin(), e.position.end(), [](float v) { return std::isfinite(v); }),
        "Invalid AP catalog orb");
    Require(c.orbs.emplace(e.id, e).second, "Duplicate AP catalog location");
  }
  Require(c.orbs.size() == 800 && !c.orbs.contains(c.agility_item), "Invalid AP catalog IDs");
  for (const auto& [index, skill] : c.skills)
    Require(!c.orbs.contains(skill.item), "AP catalog item/location ID collision");
  if (j.contains("weapons")) {
    Require(c.skills.size() == 5, "M2 catalog requires five skills");
    c.filler_item = j.at("filler_item").get<int64_t>();
    for (const auto& s : j.at("supply_points")) {
      Catalog::Supply e{s.at("id").get<int64_t>(), s.at("index").get<int>(),
          s.at("starting").get<bool>(), {s.at("position").at(0).get<float>(),
          s.at("position").at(1).get<float>(), s.at("position").at(2).get<float>()}};
      Require(e.index >= 0 && e.index < 16 && e.starting == (e.index == 15) &&
          std::all_of(e.position.begin(), e.position.end(), [](float v) { return std::isfinite(v); }) &&
          c.supplies.emplace(e.index, e).second, "Invalid supply catalog");
    }
    for (const auto& b : j.at("bosses")) {
      Catalog::Boss e{b.at("id").get<int64_t>(), b.at("native_id").get<int>(), b.at("kingpin").get<bool>()};
      Require(e.native_id >= 0 && e.native_id < 21 &&
          e.kingpin == (e.native_id == 3 || e.native_id == 11 || e.native_id == 17) &&
          c.bosses.emplace(e.native_id, e).second, "Invalid boss catalog");
    }
    std::set<int64_t> item_ids;
    for (const auto& [index, s] : c.skills) item_ids.insert(s.item);
    Require(c.filler_item != 0 && item_ids.insert(c.filler_item).second, "Invalid filler catalog");
    for (const auto& w : j.at("weapons")) {
      Catalog::Weapon e{w.at("id").get<int64_t>(), w.at("item").get<int64_t>(),
          w.at("native_id").get<int>(), w.at("index").get<int>(),
          w.at("starting").get<bool>(), w.at("name").get<std::string>()};
      Require(e.native_id >= 1 && e.native_id <= 19 && e.index == e.native_id - 1 &&
          e.starting == (e.native_id <= 3 || e.native_id == 18) &&
          item_ids.insert(e.item).second && c.weapons.emplace(e.native_id, e).second,
          "Invalid weapon catalog");
    }
    Require(c.supplies.size() == 16 && c.bosses.size() == 21 && c.weapons.size() == 19,
        "Incomplete M2 catalog");
    auto ids = c.Locations();
    Require(ids.size() == 855, "Duplicate M2 location ID");
    // Include the Agency entry in collision validation even though it has no check.
    Require(!ids.contains(c.supplies.at(15).id) && !item_ids.contains(c.supplies.at(15).id),
        "Agency location collision");
    for (auto id : item_ids) Require(!ids.contains(id), "M2 item/location collision");
  }
  return c;
}

std::set<int64_t> Catalog::Locations(bool hidden) const {
  std::set<int64_t> ids;
  for (const auto& [id, o] : orbs) if (hidden || o.type != 4) ids.insert(id);
  for (const auto& [index, s] : supplies) if (!s.starting) ids.insert(s.id);
  for (const auto& [id, w] : weapons) ids.insert(w.id);
  for (const auto& [id, b] : bosses) ids.insert(b.id);
  return ids;
}

Contract ParseSlotContract(const Catalog& catalog, std::string_view text,
                           const std::set<int64_t>& locations) {
  const auto data = nlohmann::json::parse(text);
  Contract c;
  c.binding.catalog = data.at("catalog").get<std::string>();
  c.binding.schema = data.at("schema").get<int>();
  Require(c.binding.catalog == catalog.version && c.binding.schema == 1, "Slot catalog mismatch");
  c.agility_item = data.at("agility_item").get<int64_t>();
  c.starting_agility = data.at("starting_agility").get<int>();
  c.maximum_agility = data.at("maximum_agility").get<int>();
  Require(c.agility_item == catalog.agility_item && c.starting_agility == catalog.starting_agility &&
      c.maximum_agility == catalog.maximum_agility, "Slot Agility mismatch");
  if (!catalog.skills.empty()) {
    for (const auto& s : data.at("skills"))
      Require(c.skills.emplace(s.at("index").get<int>(), SkillProgression{s.at("item").get<int64_t>(),
          s.at("starting").get<int>(), s.at("maximum").get<int>()}).second, "Duplicate slot skill");
    Require(c.skills == catalog.skills, "Slot skills mismatch");
  }
  if (!catalog.weapons.empty()) {
    for (const auto& w : data.at("weapons")) {
      const auto id = w.at("native_id").get<int>();
      Require(catalog.weapons.contains(id), "Unknown slot weapon");
      const auto& expected = catalog.weapons.at(id);
      Require(w.at("id").get<int64_t>() == expected.id && w.at("index").get<int>() == expected.index &&
          w.at("item").get<int64_t>() == expected.item && w.at("starting").get<bool>() == expected.starting &&
          c.weapons.emplace(id, WeaponProgression{expected.item, expected.starting}).second,
          "Slot weapon mismatch");
    }
    Require(c.weapons.size() == 19, "Incomplete slot weapons");
    c.filler_item = data.at("filler_item").get<int64_t>();
    Require(c.filler_item == catalog.filler_item, "Slot filler mismatch");
    c.goal = data.at("goal").get<std::string>();
    Require(c.goal == "kingpins" || c.goal == "max_skills", "Unsupported slot goal");
    std::set<int64_t> kingpins;
    for (const auto& [id, b] : catalog.bosses) if (b.kingpin) kingpins.insert(b.id);
    for (const auto& id : data.at("kingpin_locations"))
      Require(c.kingpin_locations.insert(id.get<int64_t>()).second, "Duplicate slot kingpin");
    Require(c.kingpin_locations == kingpins, "Slot kingpins mismatch");
    Require(locations == catalog.Locations(data.at("include_hidden_orbs").get<bool>()),
        "Slot locations mismatch");
  } else {
    for (auto id : locations) Require(catalog.orbs.contains(id), "Unknown slot location");
  }
  Require(!locations.empty(), "Empty slot locations");
  c.locations = locations;
  return c;
}

const Catalog& EmbeddedCatalog() {
  static const Catalog catalog = ParseCatalog(
      std::string_view(reinterpret_cast<const char*>(kCatalogJson), kCatalogJsonSize));
  return catalog;
}

void ValidateStoredContract(const Catalog& catalog, const Contract& c) {
  Require(c.binding.catalog == catalog.version && c.binding.schema == 1 &&
      c.agility_item == catalog.agility_item && c.starting_agility == catalog.starting_agility &&
      c.maximum_agility == catalog.maximum_agility && c.skills == catalog.skills,
      "Stored catalog/skills differ from embedded catalog");
  std::map<int, WeaponProgression> weapons;
  std::set<int64_t> kingpins;
  for (const auto& [id, w] : catalog.weapons) weapons.emplace(id, WeaponProgression{w.item, w.starting});
  for (const auto& [id, b] : catalog.bosses) if (b.kingpin) kingpins.insert(b.id);
  Require(c.weapons == weapons && c.filler_item == catalog.filler_item && c.kingpin_locations == kingpins,
      "Stored M2 items/kingpins differ from embedded catalog");
  if (!weapons.empty()) {
    Require((c.goal == "kingpins" || c.goal == "max_skills") &&
        (c.locations == catalog.Locations() || c.locations == catalog.Locations(false)),
        "Stored M2 goal/locations differ from embedded catalog");
  } else {
    Require(c.goal.empty(), "Unexpected stored M2 goal");
    for (auto id : c.locations) Require(catalog.orbs.contains(id), "Unknown stored orb");
  }
}
}  // namespace crackdown::ap

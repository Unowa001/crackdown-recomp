#include "catalog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace crackdown::ap;
using nlohmann::json;
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void Reject(const json& j) {
  bool rejected = false;
  try { ParseCatalog(j.dump()); } catch (const std::exception&) { rejected = true; }
  Check(rejected, "invalid catalog accepted");
}
}
int main(int argc, char** argv) {
  try {
    Check(argc == 2, "supply catalog path");
    std::ifstream input(argv[1]);
    auto j = json::parse(input);
    const auto& embedded = EmbeddedCatalog();
    Check(embedded.orbs.size() == 800, "incomplete embedded catalog");
    int agility = 0, hidden = 0;
    for (unsigned ordinal = 0; ordinal < 800; ++ordinal) {
      const auto* orb = embedded.ByOrdinal(ordinal);
      Check(orb != nullptr, "missing save ordinal");
      if (orb->type == 4) ++hidden; else ++agility;
    }
    Check(agility == 500 && hidden == 300, "orb type totals differ");
    Check(embedded.version == "crackdown-m2-dev-1" && embedded.Locations().size() == 855 &&
        embedded.Locations(false).size() == 555 && embedded.weapons.size() == 19,
        "full M2 catalog differs");
    json slot{{"catalog", j["version"]}, {"schema", 1}, {"agility_item", embedded.agility_item},
        {"starting_agility", 1}, {"maximum_agility", 5}, {"skills", j["skills"]},
        {"weapons", j["weapons"]}, {"filler_item", j["filler_item"]}, {"goal", "kingpins"},
        {"kingpin_locations", {73402003, 73402011, 73402017}}, {"include_hidden_orbs", true}};
    auto contract = ParseSlotContract(embedded, slot.dump(), embedded.Locations());
    ValidateStoredContract(embedded, contract);
    auto reject_stored = [&](Contract wrong) {
      bool rejected = false;
      try { ValidateStoredContract(embedded, wrong); } catch (const std::exception&) { rejected = true; }
      Check(rejected, "invalid offline contract accepted");
    };
    auto wrong = contract; wrong.weapons.at(4).item++; reject_stored(wrong);
    wrong = contract; wrong.weapons.at(4).starting = true; reject_stored(wrong);
    wrong = contract; wrong.skills.at(4).item++; reject_stored(wrong);
    wrong = contract; wrong.locations.erase(73403001); reject_stored(wrong);
    wrong = contract; wrong.goal = "items"; reject_stored(wrong);
    Check(contract.weapons.size() == 19 && contract.filler_item == 73411000, "slot M2 items");
    auto reject_slot = [&](json bad, const std::set<int64_t>& locations) {
      bool rejected = false;
      try { ParseSlotContract(embedded, bad.dump(), locations); }
      catch (const std::exception&) { rejected = true; }
      Check(rejected, "invalid slot accepted");
    };
    auto malformed = slot; malformed["weapons"][0]["starting"] = false;
    reject_slot(malformed, embedded.Locations());
    malformed = slot; malformed["weapons"].erase(18); reject_slot(malformed, embedded.Locations());
    malformed = slot; malformed["kingpin_locations"] = {73402003, 73402011, 73402011};
    reject_slot(malformed, embedded.Locations());
    malformed = slot; malformed["filler_item"] = 73412003; reject_slot(malformed, embedded.Locations());
    reject_slot(slot, embedded.Locations(false));
    slot["include_hidden_orbs"] = false;
    Check(ParseSlotContract(embedded, slot.dump(), embedded.Locations(false)).locations.size() == 555,
        "hidden off slot rejected");
    j["skills"] = json::array();
    for (int index = 0; index < 5; ++index)
      j["skills"].push_back({{"index", index}, {"item", embedded.agility_item + index},
                              {"starting", 1}, {"maximum", 5}});
    auto m2 = ParseCatalog(j.dump());
    Check(m2.skills.size() == 5 && m2.skills.at(4).item == embedded.agility_item + 4,
          "five-skill mapping differs");
    auto bad = j; bad["skills"][4]["index"] = 3; Reject(bad);
    bad = j; bad["skills"][4]["item"] = embedded.agility_item; Reject(bad);
    bad = j; bad["skills"][1]["item"] = embedded.orbs.begin()->first; Reject(bad);
    bad = j; bad["skills"][0]["starting"] = 2; Reject(bad);
    bad = j; bad["skills"][1]["maximum"] = 6; Reject(bad);
    bad = j; bad["skills"].erase(4); Reject(bad);
    bad = j; bad["orbs"][1]["ordinal"] = bad["orbs"][0]["ordinal"]; Reject(bad);
    bad = j; bad["orbs"].erase(799); Reject(bad);
    bad = j; bad["weapons"][0]["item"] = embedded.agility_item; Reject(bad);
    bad = j; bad["bosses"][0]["id"] = bad["orbs"][0]["id"]; Reject(bad);
    bad = j; bad["supply_points"][0]["starting"] = true; Reject(bad);
    std::cout << "catalog: 500 agility + 300 hidden, five skills, invalid contracts rejected\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

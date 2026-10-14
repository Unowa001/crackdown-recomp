#include "session_state.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
using namespace crackdown::ap;
namespace fs = std::filesystem;
namespace {
void Check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> void Reject(F action) {
  bool rejected = false;
  try { action(); } catch (const std::exception&) { rejected = true; }
  Check(rejected, "expected rejection");
}
struct Fixture {
  fs::path root, path;
  Contract contract{{"Crackdown", "test-seed", "isolated-profile", "save-uuid", "fixture-v1", 1, 0, 1},
                    900001, {910001, 910002, 910003}, 1, 5};
  Fixture() {
    std::random_device random;
    for (int i = 0; i < 16; ++i) {
      root = fs::temp_directory_path() / ("crackdown-ap-state-" + std::to_string(random()));
      if (fs::create_directory(root)) { path = root / "session.toml"; return; }
    }
    throw std::runtime_error("cannot create isolated fixture");
  }
  ~Fixture() { std::error_code ec; fs::remove_all(root, ec); }
  NetworkItem Item(int player = 1, int64_t location = 910001) const {
    return {contract.agility_item, location, player, 1};
  }
  std::string Read() const {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
  }
};
void BindingTests() {
  Fixture f;
  SessionState s(f.path, f.contract);
  const auto original = f.Read();
  auto test = [&](Contract bad) {
    Reject([&] { SessionState wrong(f.path, bad); });
    Reject([&] { s.Receive(bad.binding, 0, {f.Item()}); });
    Reject([&] { s.Observe(bad.binding, 910001); });
    Reject([&] { s.Acknowledge(bad.binding, {910001}); });
    Reject([&] { s.AgilityTarget(bad.binding, 1, true); });
    Check(f.Read() == original, "mismatch changed durable state");
  };
  auto bad = f.contract; bad.binding.seed += "-other"; test(bad);
  bad = f.contract; bad.binding.team = 1; test(bad);
  bad = f.contract; bad.binding.slot = 2; test(bad);
  bad = f.contract; bad.binding.profile += "-other"; test(bad);
  bad = f.contract; bad.binding.save += "-other"; test(bad);
  bad = f.contract; bad.binding.catalog += "-other"; test(bad);
  bad = f.contract; bad.binding.schema = 2; test(bad);
  bad = f.contract; bad.agility_item++; Reject([&] { SessionState wrong(f.path, bad); });
  bad = f.contract; bad.locations.insert(910004); Reject([&] { SessionState wrong(f.path, bad); });
  bad = f.contract; bad.starting_agility = 2; Reject([&] { SessionState wrong(f.path, bad); });
  Check(!s.AgilityTarget(f.contract.binding, 1, true), "grant before inventory known");
}
void ReceiptTests() {
  Fixture f;
  SessionState s(f.path, f.contract);
  auto& b = f.contract.binding;
  Check(s.Receive(b, 2, {f.Item()}) == Receipt::resync_required, "initial gap");
  Check(s.Inventory().empty(), "gap granted an item");
  Check(s.Receive(b, 0, {f.Item()}) == Receipt::replaced, "index-zero replaces");
  Check(s.AgilityTarget(b, 1, true) == 2, "own item included");
  Check(s.Receive(b, 1, {f.Item(2, 910002)}) == Receipt::appended, "remote item appended");
  Check(s.AgilityTarget(b, 1, true) == 3, "remote item included");
  Check(s.Receive(b, 1, {f.Item(2, 910002)}) == Receipt::resync_required, "duplicate requires resync");
  Check(s.Inventory().size() == 2 && !s.AgilityTarget(b, 1, true), "duplicate cannot grant");
  Check(s.Receive(b, 2, {f.Item()}) == Receipt::resync_required, "wait for full resync");
  s.Receive(b, 0, {f.Item(), f.Item(2, 910002)});
  Check(s.Inventory().size() == 2 && !s.NeedsResync(), "resync recovers");
  s.Receive(b, 0, {f.Item(), f.Item(2, 910002)});
  Check(s.AgilityTarget(b, 3, true) == std::nullopt, "full replay does not add grants");
  s.Receive(b, 0, {});
  Check(s.Inventory().empty() && s.AgilityTarget(b, 3, true) == 1, "empty replacement abandons history");
  s.Receive(b, 0, std::vector<NetworkItem>(8, f.Item()));
  Check(s.AgilityTarget(b, 1, true) == 5, "native cap");
  const auto before = f.Read();
  Reject([&] { s.Receive(b, -1, {f.Item()}); });
  auto unknown = f.Item(); unknown.item++;
  Reject([&] { s.Receive(b, 0, {unknown}); });
  auto invalid = f.Item(); invalid.flags = 8;
  Reject([&] { s.Receive(b, 0, {invalid}); });
  Reject([&] { s.Receive(b, 0, std::vector<NetworkItem>(100001, f.Item())); });
  Check(f.Read() == before && s.Inventory().size() == 8, "invalid receipt changed state");
}
void RecoveryTests() {
  Fixture f;
  auto& b = f.contract.binding;
  {
    SessionState s(f.path, f.contract);
    s.Receive(b, 0, {f.Item(), f.Item(2)});
    s.Observe(b, 910001);
  }
  SessionState restored(f.path, f.contract);
  Check(restored.Inventory().size() == 2, "inventory restored after restart");
  Check(restored.AgilityTarget(b, 1, true) == 3, "older save restores permanent progression");
  Check(!restored.AgilityTarget(b, 3, true), "already reconciled state is idempotent");
  Check(!restored.AgilityTarget(b, 1, false), "menu/loading/death cannot grant");
  Reject([&] { restored.AgilityTarget(b, 0, true); });
  Check(restored.PendingChecks() == std::vector<int64_t>{910001}, "offline check survived restart");
  restored.Receive(b, 0, {f.Item(), f.Item(2)});
  Check(!restored.AgilityTarget(b, 3, true), "reconnect inventory cannot duplicate grants");
}
void SkillTests() {
  Fixture f;
  for (int index = 0; index < 5; ++index)
    f.contract.skills.emplace(index, SkillProgression{f.contract.agility_item + index, 1, 5});
  SessionState s(f.path, f.contract);
  const auto& b = f.contract.binding;
  std::vector<NetworkItem> receipts;
  for (int index = 0; index < 5; ++index)
    for (int count = 0; count < index; ++count)
      receipts.push_back({f.contract.skills.at(index).item, 910001, 2, 1});
  s.Receive(b, 0, receipts);
  for (int index = 0; index < 5; ++index) {
    Check(s.SkillTarget(b, index, 5, true) == (index == 4 ? std::optional<int>{} : index + 1),
          "mixed receipts crossed skill indices");
    Check(!s.SkillTarget(b, index, index + 1, true), "matching level regranted");
    Check(!s.SkillTarget(b, index, 1, false), "grant before gameplay ready");
  }
  Check(SessionState::ReadContract(f.path) == f.contract, "five-skill contract not persisted");
  SessionState restored(f.path, f.contract);
  for (int index = 1; index < 5; ++index)
    Check(restored.SkillTarget(b, index, 1, true) == index + 1, "offline skill restore");
  Check(restored.Receive(b, 1, receipts) == Receipt::resync_required, "duplicate mixed batch accepted");
  for (int index = 0; index < 5; ++index)
    Check(!restored.SkillTarget(b, index, 1, true), "skill granted during resync");
  restored.Receive(b, 0, receipts);
  Check(restored.Inventory() == receipts, "full mixed replay duplicated inventory");
  for (int index = 0; index < 5; ++index)
    receipts.push_back({f.contract.skills.at(index).item, 910002, 1, 1});
  restored.Receive(b, 0, receipts);
  Check(restored.SkillTarget(b, 4, 1, true) == 5, "Firearms exceeds cap");
  auto bad = f.contract;
  bad.skills.at(1).item = bad.skills.at(2).item;
  Reject([&] { SessionState wrong(f.root / "duplicate.toml", bad); });
  bad = f.contract; bad.skills.erase(4);
  Reject([&] { SessionState wrong(f.root / "missing.toml", bad); });
  bad = f.contract; bad.skills.at(3).maximum = 6;
  Reject([&] { SessionState wrong(f.root / "bounds.toml", bad); });
  Reject([&] { restored.SkillTarget(b, 5, 1, true); });
  Fixture legacy;
  SessionState m1(legacy.path, legacy.contract);
  m1.Receive(legacy.contract.binding, 0, {legacy.Item()});
  Check(!m1.SkillTarget(legacy.contract.binding, 1, 1, true), "M1 touched another skill");
}
void WeaponTests() {
  Fixture f;
  for (int i = 0; i < 5; ++i) f.contract.skills.emplace(i, SkillProgression{f.contract.agility_item + i, 1, 5});
  for (int id = 1; id <= 19; ++id)
    f.contract.weapons.emplace(id, WeaponProgression{920100 + id, id <= 3 || id == 18});
  f.contract.filler_item = 921000;
  f.contract.goal = "kingpins";
  f.contract.kingpin_locations = f.contract.locations;
  SessionState s(f.path, f.contract);
  const auto& b = f.contract.binding;
  Check(!s.WeaponTarget(b, 1, true), "grant before full inventory");
  std::vector<NetworkItem> items{{920104, 910001, 2, 2}, {921000, 910002, 1, 0}};
  s.Receive(b, 0, items);
  Check(s.WeaponTarget(b, 1, true) == true && s.WeaponTarget(b, 18, true) == true,
        "starting inventory unavailable");
  Check(s.WeaponTarget(b, 4, true) == true && s.WeaponTarget(b, 5, true) == false,
        "weapon receipt crossed IDs");
  Check(s.Observed().empty(), "starting/received weapons manufactured deposits");
  Check(!s.WeaponTarget(b, 4, false), "weapon grant before readiness");
  Check(s.Receive(b, 1, items) == Receipt::resync_required && !s.WeaponTarget(b, 4, true),
        "grant during resync");
  s.Receive(b, 0, items);
  SessionState restored(f.path, f.contract);
  Check(SessionState::ReadContract(f.path) == f.contract && restored.WeaponTarget(b, 4, true) == true,
        "offline weapon recovery");
  restored.Receive(b, 0, {{921000, 910001, 2, 0}});
  Check(restored.WeaponTarget(b, 4, true) == false && restored.Inventory().size() == 1 &&
      restored.Observed().empty(), "replacement/filler modified deposit observations");
  Reject([&] { restored.Receive(b, 0, {{73412003, 910001, 2, 1}}); });
  Reject([&] { restored.WeaponTarget(b, 20, true); });
}
void GoalTests() {
  Fixture f;
  for (int i = 0; i < 5; ++i) f.contract.skills.emplace(i, SkillProgression{f.contract.agility_item + i, 1, 5});
  for (int id = 1; id <= 19; ++id) f.contract.weapons.emplace(id, WeaponProgression{920100 + id, id <= 3 || id == 18});
  f.contract.filler_item = 921000;
  f.contract.goal = "kingpins";
  f.contract.kingpin_locations = f.contract.locations;
  const auto& b = f.contract.binding;
  const std::string h1(64, 'a'), h2(64, 'b');
  SessionState s(f.path, f.contract);
  s.Receive(b, 0, {{920104, 910001, 1, 2}, {921000, 910001, 2, 0}});
  for (auto id : f.contract.kingpin_locations) s.Observe(b, id);
  s.Acknowledge(b, f.contract.locations, true);
  Check(!s.GoalComplete(), "items/checks/acks manufactured native victory");
  s.RecordSave(b, h1, {910001, 910002}, true);
  Check(!s.GoalComplete(), "incomplete kingpins or max skills satisfied kingpin goal");
  const auto held = f.root.string() + "-held";
  fs::rename(f.root, held);
  { std::ofstream block(f.root); block << "not a directory"; }
  Reject([&] { s.RecordSave(b, h2, f.contract.kingpin_locations); });
  Check(!s.GoalComplete(), "victory exposed before durable native-save witness");
  fs::remove(f.root); fs::rename(held, f.root);
  s.RecordSave(b, h2, f.contract.kingpin_locations);
  Check(s.GoalComplete(), "native kingpin save did not complete goal");
  SessionState reload(f.path, f.contract);
  Check(reload.GoalComplete(), "offline/reconnect lost native victory");
  reload.Receive(b, 0, {});
  reload.Acknowledge(b, {}, true);
  Check(reload.AcceptLoad(b, h1, {}) && reload.GoalComplete(), "older bound save erased durable victory");
  auto wrong = b; wrong.seed = "another";
  Reject([&] { reload.RecordSave(wrong, h2, f.contract.kingpin_locations); });
  Fixture dev;
  dev.contract = f.contract; dev.contract.goal = "max_skills";
  SessionState max(dev.path, dev.contract);
  max.RecordSave(b, h1, f.contract.kingpin_locations, false);
  Check(!max.GoalComplete(), "kingpins satisfied max-skills development goal");
  max.RecordSave(b, h1, {}, true);
  Check(max.GoalComplete(), "native max-skills save not recognized");
}
void CheckTests() {
  Fixture f;
  auto& b = f.contract.binding;
  SessionState s(f.path, f.contract);
  Check(s.Observe(b, 910001) && !s.Observe(b, 910001), "one durable observation");
  Check(s.PendingChecks() == std::vector<int64_t>{910001}, "sending alone is not acknowledgment");
  s.Acknowledge(b, {910002}); // A partner can check before local observation.
  Check(s.PendingChecks() == std::vector<int64_t>{910001}, "unrelated ack preserves pending");
  s.Observe(b, 910002);
  Check(s.PendingChecks() == std::vector<int64_t>{910001}, "partner check not resent");
  s.Acknowledge(b, {910001});
  s.Acknowledge(b, {910001});
  Check(s.PendingChecks().empty(), "confirmed check stops retry");
  s.Observe(b, 910003);
  SessionState reload(f.path, f.contract);
  Check(reload.PendingChecks() == std::vector<int64_t>{910003}, "durable ack/local distinction");
  Reject([&] { s.Observe(b, 919999); });
  Reject([&] { s.Acknowledge(b, {910003, 919999}); });
  Check(s.PendingChecks() == std::vector<int64_t>{910003}, "invalid ack is transactional");
}
void PersistenceTests() {
  Fixture f;
  auto& b = f.contract.binding;
  SessionState s(f.path, f.contract);
  s.Receive(b, 0, {f.Item()});
  s.Observe(b, 910001);
  const auto before = f.Read();
  // Replace parent directory with a regular file to force persistence failure.
  const auto held = f.root.string() + "-held";
  fs::rename(f.root, held);
  { std::ofstream block(f.root); block << "not a directory"; }
  Reject([&] { s.Receive(b, 1, {f.Item()}); });
  Reject([&] { s.Observe(b, 910002); });
  Reject([&] { s.Acknowledge(b, {910001}); });
  Check(s.Inventory().size() == 1 && s.PendingChecks() == std::vector<int64_t>{910001},
        "failed commit exposed inventory/observation/ack changes");
  fs::remove(f.root);
  fs::rename(held, f.root);
  Check(f.Read() == before, "failed commit damaged original");
  s.Observe(b, 910002);
  Check(s.PendingChecks() == std::vector<int64_t>({910001, 910002}), "retry after write failure");
  auto corrupt = [&](const std::string& text) {
    { std::ofstream out(f.path, std::ios::binary | std::ios::trunc); out << text; }
    Reject([&] { SessionState bad(f.path, f.contract); });
    Check(f.Read() == text, "corrupt state silently overwritten");
  };
  corrupt("format = 77\n");
  corrupt("this is truncated [[[");
  auto missing = before;
  auto offset = missing.find("inventory_known");
  Check(offset != std::string::npos, "fixture missing key");
  missing.erase(offset, missing.find('\n', offset) - offset);
  corrupt(missing);
  for (const auto& entry : fs::directory_iterator(f.root))
    Check(entry.path() == f.path, "temporary file leaked");
}
void LineageTests() {
  Fixture f;
  auto& b = f.contract.binding;
  const std::string h1(64, 'a'), h2(64, 'b'), h3(64, 'd'), foreign(64, 'c');
  const int64_t t0 = 1'800'000'000, window = SessionState::kPendingWindow;
  Check(!SessionState::ReadContract(f.path), "absent sidecar has a contract");
  {
    SessionState s(f.path, f.contract);
    Check(!s.HasNativeSave() && !s.AcceptLoad(b, foreign, t0), "unbound load accepted");
    s.BeginSave(b, t0);
    Check(s.HasNativeSave(), "pending save not durable");
    s.RecordSave(b, h1);
    Check(s.AcceptLoad(b, h1, {}) && !s.AcceptLoad(b, foreign, t0 + 1), "lineage membership");
    Reject([&] { s.RecordSave(b, "not-a-hash"); });
    Reject([&] { s.BeginSave({"Crackdown", "other", "isolated-profile", "save-uuid", "fixture-v1", 1, 0, 1}, t0); });
    s.BeginSave(b, t0 + 100); // crash after native write, before RecordSave
  }
  Check(SessionState::ReadContract(f.path) == f.contract, "offline reopen contract");
  SessionState s(f.path, f.contract);
  // A pending marker admits only a file written just after it (not a copied-in save).
  Check(!s.AcceptLoad(b, foreign, t0 + 50), "file older than the pending save accepted");
  Check(!s.AcceptLoad(b, foreign, t0 + 100 + window + 1), "file long after the pending save accepted");
  Check(!s.AcceptLoad(b, foreign, std::nullopt), "file without a timestamp accepted");
  Check(s.AcceptLoad(b, h2, t0 + 103), "interrupted pending write accepted once");
  Check(!s.AcceptLoad(b, foreign, t0 + 104), "pending write accepted twice");
  Check(s.AcceptLoad(b, h1, {}) && s.AcceptLoad(b, h2, {}), "older snapshot stays in lineage");
  SessionState reload(f.path, f.contract);
  Check(reload.AcceptLoad(b, h2, {}) && !reload.AcceptLoad(b, foreign, t0 + 104), "lineage persisted");
  // Sidecars written before save_pending_at existed never admit an unknown hash.
  reload.BeginSave(b, t0 + 200);
  auto text = f.Read();
  auto at = text.find("save_pending_at");
  Check(at != std::string::npos, "pending time not persisted");
  text.erase(at, text.find('\n', at) - at + 1);
  { std::ofstream out(f.path, std::ios::binary | std::ios::trunc); out << text; }
  SessionState legacy(f.path, f.contract);
  Check(!legacy.AcceptLoad(b, h3, t0 + 201), "untimed pending write accepted");
}
void AcknowledgeTests() {
  Fixture f;
  auto& b = f.contract.binding;
  SessionState s(f.path, f.contract);
  s.Observe(b, 910001);
  s.Observe(b, 910002);
  s.Acknowledge(b, {910001, 910002});
  Check(s.PendingChecks().empty(), "acknowledged");
  s.Acknowledge(b, {910002}, true);  // server restored from an older save
  Check(s.PendingChecks() == std::vector<int64_t>{910001}, "forgotten check pending again");
  s.Acknowledge(b, {910003});
  Check(s.PendingChecks() == std::vector<int64_t>{910001}, "RoomUpdate only adds");
}
}int main(int argc, char** argv) {
  try {
    Check(argc == 2, "supply a case");
    std::string name = argv[1];
    if (name == "binding") BindingTests();
    else if (name == "receipts") ReceiptTests();
    else if (name == "recovery") RecoveryTests();
    else if (name == "checks") CheckTests();
    else if (name == "persistence") PersistenceTests();
    else if (name == "lineage") LineageTests();
    else if (name == "acknowledge") AcknowledgeTests();
    else if (name == "skills") SkillTests();
    else if (name == "weapons") WeaponTests();
    else if (name == "goal") GoalTests();
    else throw std::runtime_error("unknown case");
    std::cout << name << " passed\n";
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

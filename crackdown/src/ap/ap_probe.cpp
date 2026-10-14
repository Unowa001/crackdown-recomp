// Opt-in game-contract probe, not an AP session/client.
// Evidence: docs/re-ap-probe.md and docs/re-discovery-randomizer.md.
// 82313968 clears pickup+288[player]; 82543A28/82543BB0 award bonus/ordinary
// XP. 82543DB0 is the native level setter used by console callback 825087A8.
#include "generated/default/crackdown_init.h"

#include <array>
#include <atomic>
#include <bit>
#include <rex/cvar.h>
#include <rex/hook.h>

#include "bridge.h"
#include "catalog.h"
#include <string>
#include <set>
#include "../input_replay.h"

REXCVAR_DEFINE_BOOL(ap_probe, false, "Crackdown",
                    "Developer: log orb, supply and weapon transitions (no AP connection)");
REXCVAR_DEFINE_BOOL(ap_probe_no_xp, false, "Crackdown",
                    "Developer, requires ap_probe: suppress primary-agent XP; use an isolated save");
REXCVAR_DEFINE_INT32(ap_probe_agility_level, 0, "Crackdown",
                     "Developer, requires ap_probe: set native Agility 1-5 after first collected orb; 0 disables");
REXCVAR_DEFINE_INT32(ap_probe_skill_index, 0, "Crackdown",
                     "Developer, requires ap_probe_skill_level: native skill index 0-4");
REXCVAR_DEFINE_INT32(ap_probe_skill_level, 0, "Crackdown",
                     "Developer, requires ap_probe: set selected skill 1-5 after first collected orb; 0 disables");
REXCVAR_DEFINE_INT32(ap_probe_all_skills_level, 0, "Crackdown",
                     "Developer, requires ap_probe: set all five native skills 1-5 after first collected orb; 0 disables");
REXCVAR_DEFINE_INT32(ap_probe_carried_weapon_id, 0, "Crackdown",
                     "Developer, requires ap_probe and an isolated save: native addweapon for ID 1-19, 20=all; 0 disables");
REXCVAR_DEFINE_INT32(ap_probe_boss_defeat_id, -1, "Crackdown",
                     "Developer, requires ap_probe and an isolated save: native callback for boss ID 0-20, 21=each spawned boss (not a combat kill); -1 disables");

namespace {
// Only accept the primary-player SkillSet layout independently observed in the
// supported XEX. Other characters and menu skill objects keep vanilla behavior.
uint32_t PrimarySkills(uint8_t* base) {
  const uint32_t player = REX_LOAD_U32(0x82DE3F3C);
  if (!player) return 0;
  const uint32_t component = REX_LOAD_U32(player + 292);
  if (!component || component < 32) return 0;
  const uint32_t character = component - 32;
  const uint32_t vtable = REX_LOAD_U32(character);
  if (vtable != 0x8208D850 || REX_LOAD_U32(vtable + 1220) != 0x82299350) return 0;
  const uint32_t skills = character + 9056;
  return REX_LOAD_U32(skills + 8) == character ? skills : 0;
}

bool SuppressXP(PPCContext& ctx, uint8_t* base, const char* bank) {
  if (!REXCVAR_GET(ap_probe) || !REXCVAR_GET(ap_probe_no_xp)) return false;
  const auto skills = PrimarySkills(base);
  if (!skills || ctx.r3.u32 != skills || ctx.r4.u32 >= 5) return false;
  REXLOG_INFO("ap_probe: suppressed {} XP skill={} amount={:.3f}", bank, ctx.r4.u32,
              ctx.f1.f64);
  return true;
}

// One explicit diagnostic grant per process. Permanent AP inventory reconciliation
// belongs to the future bound session; this deliberately makes no recovery claim.
std::atomic<bool> g_granted{false};
void ProbeGrant(PPCContext& ctx, uint8_t* base) {
  const int all_level = REXCVAR_GET(ap_probe_all_skills_level);
  const int selected_level = REXCVAR_GET(ap_probe_skill_level);
  const int level = all_level ? all_level : selected_level ? selected_level : REXCVAR_GET(ap_probe_agility_level);
  const int selected_index = selected_level ? REXCVAR_GET(ap_probe_skill_index) : 0;
  if (level < 1 || level > 5 || (!all_level && (selected_index < 0 || selected_index > 4))) return;
  const uint32_t skills = PrimarySkills(base);
  if (!skills || g_granted.exchange(true)) return;
  const int first = all_level ? 0 : selected_index;
  const int last = all_level ? 4 : selected_index;
  for (int index = first; index <= last; ++index) {
    // Preserve the wrapped function's return/register effects. Use its game-thread
    // guest stack to run the same setter/control argument as 825087A8.
    auto grant = ctx;
    grant.r3.u64 = skills;
    const int before = REX_LOAD_U8(skills + 17 + index);
    grant.r4.u64 = static_cast<uint32_t>(index);
    grant.r5.u64 = static_cast<uint32_t>(level);
    grant.r6.u64 = 1;
    __imp__sub_82543DB0(grant, base);
    REXLOG_INFO("ap_probe: native skill grant index={} before={} target={} progression={} base={} bonus={} ordinary_xp={} bonus_xp={}",
                index, before, level, REX_LOAD_U8(skills + 17 + index),
                REX_LOAD_U8(skills + 12 + index), REX_LOAD_U8(skills + 22 + index),
                std::bit_cast<float>(REX_LOAD_U32(skills + 28 + 4 * index)),
                std::bit_cast<float>(REX_LOAD_U32(skills + 48 + 4 * index)));
  }
}
}  // namespace

void crackdown::ap::ProbeNativeAdapters(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(ap_probe) || CharacterUpdateCount() < 60 || !PrimarySkills(base)) return;
  static bool weapon_done = false;
  static std::set<int> bosses_done;
  const int requested_weapon = REXCVAR_GET(ap_probe_carried_weapon_id);
  if (!weapon_done && requested_weapon >= 1 && requested_weapon <= 20) {
    weapon_done = true;
    const int first = requested_weapon == 20 ? 1 : requested_weapon;
    const int last = requested_weapon == 20 ? 19 : requested_weapon;
    for (int weapon_id = first; weapon_id <= last; ++weapon_id) {
      const auto& weapon = EmbeddedCatalog().weapons.at(weapon_id);
      const auto& name = weapon.name;
      auto call = ctx;
      call.r1.u64 -= 1024;
      // 823925C0's resource hash table; match the weapon definition class and
      // display name (82354108), since console lookup uses a different internal
      // resource name. Only diagnostics enumerate this supported-build table.
      uint32_t definition = 0;
      const uint32_t resources = REX_LOAD_U32(0x82DE267C);
      const uint32_t type = REX_LOAD_U32(0x8306BBFC);
      const uint32_t table = resources ? resources + 102464 : 0;
      const uint32_t mask = table ? REX_LOAD_U32(table + 0x13024) : 0;
      if (type && table && mask < 8192) {
        for (uint32_t bucket = 0; bucket <= mask && !definition; ++bucket) {
          uint32_t node = REX_LOAD_U32(table + 0xE028 + 20 * bucket);
          for (int steps = 0; node && steps < 128 && !definition; ++steps) {
            const uint32_t candidate = REX_LOAD_U32(node + 20);
            const uint32_t descriptor = candidate ? REX_LOAD_U32(candidate + 56) : 0;
            if (descriptor && REX_LOAD_U32(descriptor) == REX_LOAD_U32(type)) {
              const uint32_t text = REX_LOAD_U32(candidate + 548) >= 8
                  ? REX_LOAD_U32(candidate + 528) : candidate + 528;
              bool matches = text != 0;
              for (size_t i = 0; matches && i <= name.size(); ++i)
                matches = REX_LOAD_U16(text + 2 * i) == (i < name.size() ? static_cast<unsigned char>(name[i]) : 0);
              if (matches) definition = candidate;
            }
            node = REX_LOAD_U32(node + 4);
          }
        }
      }
      if (definition) {
        call.r3.u64 = REX_LOAD_U32(0x82DE3F3C);
        call.r4.u64 = definition;
        call.r5.u64 = 1; call.r6.u64 = 0; call.r7.u64 = 0;
        __imp__sub_822B7210(call, base);
      }
      if (definition && (weapon_id == 3 || weapon_id >= 17)) {
        // Native safehouse loadout selection (82644B38, category r5=2) owns the
        // active throwable at character+6308. addweapon alone adds inventory but
        // does not replace that selection. This remains a diagnostic acquisition;
        // checks still require the subsequent ordinary supply deposit caller.
        const uint32_t manager = REX_LOAD_U32(0x835D3F84);
        const uint32_t begin = manager ? REX_LOAD_U32(manager + 12) : 0;
        const uint32_t end = manager ? REX_LOAD_U32(manager + 16) : 0;
        const uint32_t entry = begin + 112 * weapon.index;
        if (begin && end >= begin && end - begin == 19 * 112 && REX_LOAD_U32(entry + 12) == weapon_id &&
            REX_LOAD_U32(entry + 16) == 2) {
          call.r3.u64 = REX_LOAD_U32(0x82DE3F3C); call.r4.u64 = entry;
          call.r5.u64 = 2; call.r6.u64 = 1;
          __imp__sub_82644B38(call, base);
        }
      }
      REXLOG_INFO("ap_probe: carried weapon injection native_id={} definition={:08X}", weapon_id, definition);
    }
  }
  const int boss_id = REXCVAR_GET(ap_probe_boss_defeat_id);
  if (boss_id >= 0 && boss_id <= 21) {
    const uint32_t manager = REX_LOAD_U32(0x82DE27F4);
    if (!manager || REX_LOAD_U32(manager + 192) != manager + 208 + 21 * 4) return;
    for (int i = 0; i < 21; ++i) {
      const uint32_t boss = REX_LOAD_U32(manager + 208 + 4 * i);
      if (!boss || REX_LOAD_U32(boss) != 0x820A513C) continue;
      const int id = static_cast<int>(REX_LOAD_U32(boss + 32));
      if (id < 0 || id > 20 || (boss_id != 21 && id != boss_id) || bosses_done.contains(id)) continue;
      const uint32_t actor = REX_LOAD_U32(boss + 160);
      if (!actor || REX_LOAD_U8(boss + 148) != 0) continue;
      bosses_done.insert(id);
      auto call = ctx;
      call.r3.u64 = boss; call.r4.u64 = REX_LOAD_U32(0x82DE3F3C); call.r5.u64 = actor;
      REXLOG_WARN("ap_probe: diagnostic boss callback native_id={} actor={:08X}; not a combat kill", id, actor);
      sub_823CBA98(call, base);
      return;
    }
  }
}

// The AP bridge disables all gameplay XP for the primary agent in a bound
// randomized save (owner decision: skills come only from Progressive items).
REX_HOOK_RAW(sub_82543A28) {
  if (crackdown::ap::SuppressXP(ctx.r3.u32, base)) return;
  if (!SuppressXP(ctx, base, "bonus")) __imp__sub_82543A28(ctx, base);
}

REX_HOOK_RAW(sub_82543BB0) {
  if (crackdown::ap::SuppressXP(ctx.r3.u32, base)) return;
  if (!SuppressXP(ctx, base, "ordinary")) __imp__sub_82543BB0(ctx, base);
}

REX_HOOK_RAW(sub_82313968) {
  if (!REXCVAR_GET(ap_probe)) {
    __imp__sub_82313968(ctx, base);
    return;
  }
  const uint32_t manager = REX_LOAD_U32(0x82DE3F34);
  const uint32_t count = manager ? REX_LOAD_U32(manager + 4012) : 0;
  if (count != 800) {
    __imp__sub_82313968(ctx, base);
    return;
  }
  struct Orb { uint32_t address; std::array<uint8_t, 2> available; };
  std::array<Orb, 800> before{};
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t address = REX_LOAD_U32(manager + 12 + 4 * i);
    if (!address) {
      __imp__sub_82313968(ctx, base);
      return;
    }
    before[i] = {address, {REX_LOAD_U8(address + 288), REX_LOAD_U8(address + 289)}};
  }
  __imp__sub_82313968(ctx, base);
  if (REX_LOAD_U32(0x82DE3F34) != manager || REX_LOAD_U32(manager + 4012) != count) return;
  bool primary_collected = false;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& orb = before[i];
    if (REX_LOAD_U32(manager + 12 + 4 * i) != orb.address) continue;
    for (uint32_t player = 0; player < 2; ++player) {
      if (orb.available[player] == 1 && REX_LOAD_U8(orb.address + 288 + player) == 0) {
        REXLOG_INFO("ap_probe: orb collected ordinal={} type={} player={}", i,
                    REX_LOAD_U32(orb.address + 264), player);
        primary_collected |= player == 0;
      }
    }
  }
  if (primary_collected) ProbeGrant(ctx, base);
}

// Raw unlock transition probe. This is shared by capture and UI/load paths, so
// retain LR provenance and do not turn arbitrary calls into AP checks.
REX_HOOK_RAW(sub_823911A0) {
  const uint32_t player = ctx.r4.u32;
  const uint32_t index = ctx.r5.u32;
  const uint32_t caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_823911A0(ctx, base);
  crackdown::ap::OnSupplyCaptured(player, index, caller, ctx.r3.u32 != 0, base);
  if (REXCVAR_GET(ap_probe) && player < 2 && index < 40 && ctx.r3.u32) {
    REXLOG_INFO("ap_probe: supply unlocked index={} player={} caller={:08X}", index,
                player, caller);
  }
}

// Native deposit scan used by 82642950 on supply-point entry. Record transitions
// in its 112-byte armory records, leaving all native availability/HUD effects intact.
REX_HOOK_RAW(sub_82642760) {
  if (crackdown::ap::HandleWeaponDeposit(ctx, base)) return;
  if (!REXCVAR_GET(ap_probe) || ctx.r4.u32 != REX_LOAD_U32(0x82DE3F3C)) {
    __imp__sub_82642760(ctx, base);
    return;
  }
  const uint32_t manager = REX_LOAD_U32(0x835D3F84);
  const uint32_t begin = manager ? REX_LOAD_U32(manager + 12) : 0;
  const uint32_t end = manager ? REX_LOAD_U32(manager + 16) : 0;
  const uint32_t count = end >= begin ? (end - begin) / 112 : 0;
  if (!begin || !count || count > 64 || end - begin != count * 112) {
    __imp__sub_82642760(ctx, base);
    return;
  }
  std::array<uint8_t, 64> available{};
  for (uint32_t i = 0; i < count; ++i) available[i] = REX_LOAD_U8(begin + 112 * i + 9);
  __imp__sub_82642760(ctx, base);
  if (REX_LOAD_U32(manager + 12) != begin || REX_LOAD_U32(manager + 16) != end) return;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = begin + 112 * i;
    if (!available[i] && REX_LOAD_U8(entry + 9) == 1) {
      REXLOG_INFO("ap_probe: weapon deposited index={} native_id={}", i,
                  REX_LOAD_U32(entry + 12));
    }
  }
}

// In-process Archipelago bridge (bridge.cpp).
#pragma once
#include <cstdint>
#include <filesystem>

struct PPCContext;

namespace crackdown::ap {
// Opens this profile's AP sidecar and starts the client when --ap_server is set.
void Initialize(const std::filesystem::path& user_data_root);
void Shutdown();
// Game thread, once per frame from the existing sub_826A7698 hook.
void OnFrame(PPCContext& ctx, uint8_t* base);
// True when XP to this SkillSet must be dropped (primary agent, bound save).
bool SuppressXP(uint32_t skillset, uint8_t* base);
void OnSupplyCaptured(uint32_t player, uint32_t index, uint32_t caller, bool changed, uint8_t* base);
// Runs the whole native deposit body with AP armory availability isolated.
bool HandleWeaponDeposit(PPCContext& ctx, uint8_t* base);
void ProbeNativeAdapters(PPCContext& ctx, uint8_t* base); // explicitly opt-in diagnostics
}  // namespace crackdown::ap

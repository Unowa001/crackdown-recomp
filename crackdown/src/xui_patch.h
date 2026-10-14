// Verified XUIPAT1 application and the optional PC-menu runtime mount.
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, pc_menu_mod);

namespace rex { class Runtime; }

namespace crackdown {
// Throws on malformed patches, size/hash mismatch, or excessive allocation.
std::vector<uint8_t> ApplyXuiPatch(std::span<const uint8_t> original,
                                 std::span<const uint8_t> patch);
// Called once before guest launch. Failure disables routing and warns once.
void PreparePCMenu(rex::Runtime* runtime, const std::filesystem::path& game_root,
                   const std::filesystem::path& cache_root);
// Guest system-heap ANSI paths. Immutable for the lifetime of the title.
uint32_t PCMenuGuestPath();
uint32_t PCSceneGuestPath();
void DisablePCMenu(std::string_view reason);
}  // namespace crackdown

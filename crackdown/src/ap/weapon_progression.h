#pragma once
#include <cstdint>

namespace crackdown::ap {
// Permanent armory availability; deliberately contains no deposit observation.
struct WeaponProgression {
  int64_t item = 0;
  bool starting = false;
  bool operator==(const WeaponProgression&) const = default;
};
}

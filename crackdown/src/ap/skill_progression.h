#pragma once
#include <cstdint>

namespace crackdown::ap {
struct SkillProgression {
  int64_t item = 0;
  int starting = 1, maximum = 5;
  bool operator==(const SkillProgression&) const = default;
};
}  // namespace crackdown::ap

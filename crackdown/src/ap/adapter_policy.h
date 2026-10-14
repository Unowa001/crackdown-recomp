#pragma once
#include <cstdint>

namespace crackdown::ap {
constexpr bool NaturalSupplyCapture(uint32_t player, uint32_t index, uint32_t lr, bool changed) {
  return player == 0 && index < 15 && lr == 0x82533490 && changed;
}
constexpr bool NaturalDeposit(uint32_t player, uint32_t primary, uint32_t lr) {
  return primary != 0 && player == primary && lr == 0x826429A0;
}
constexpr bool DepositResolver(uint32_t lr) {
  return lr == 0x826427FC || lr == 0x8264284C;
}
}

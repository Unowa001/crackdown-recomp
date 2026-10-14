#include "adapter_policy.h"
#include <stdexcept>
using namespace crackdown::ap;
int main() {
  auto check = [](bool ok) { if (!ok) throw std::runtime_error("adapter provenance failed"); };
  check(NaturalSupplyCapture(0, 0, 0x82533490, true));
  check(NaturalSupplyCapture(0, 14, 0x82533490, true));
  check(!NaturalSupplyCapture(1, 0, 0x82533490, true));
  check(!NaturalSupplyCapture(0, 15, 0x82533490, true));
  check(!NaturalSupplyCapture(0, 0, 0x82533494, true));
  check(!NaturalSupplyCapture(0, 0, 0x82533490, false));
  check(NaturalDeposit(1234, 1234, 0x826429A0));
  check(!NaturalDeposit(1234, 1234, 0x8264299C));
  check(!NaturalDeposit(4321, 1234, 0x826429A0));
  check(!NaturalDeposit(0, 0, 0x826429A0));
  check(DepositResolver(0x826427FC) && DepositResolver(0x8264284C));
  check(!DepositResolver(0x826427F8) && !DepositResolver(0x826429A0));
}

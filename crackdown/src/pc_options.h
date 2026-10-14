#pragma once
#include <functional>

namespace crackdown {
// Installed before guest launch; callbacks enqueue work and return queue success.
void SetPCMenuQuitAction(std::function<bool()> quit);
}  // namespace crackdown

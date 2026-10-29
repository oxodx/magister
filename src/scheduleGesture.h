#pragma once

#include <cmath>

namespace magister {
// Returns +1 for a left swipe towards the next day, -1 for the previous day.
// Keeping recognition independent from SDL makes it easy to test and tune.
inline int daySwipe(float dx, float dy, float threshold) {
  if (std::abs(dx) < threshold || std::abs(dx) < std::abs(dy) * 1.25f)
    return 0;
  return dx < 0 ? 1 : -1;
}
} // namespace magister

#pragma once
#include <algorithm>
#include <cmath>

namespace magister {
struct ScrollPhysics {
  float offset = 0;
  float maximum = 0;

  void reset() { offset = maximum = 0; }

  // Integrate exponential friction analytically, rather than approximating
  // displacement with v * dt. Travel is consistent at 60, 90 and 120 Hz.
  float advance(float dragDelta, float &velocity, bool fingerDown, float dt) {
    float movement = dragDelta;
    if (!fingerDown) {
      const float decay = std::exp(-7.0f * std::clamp(dt, 0.0f, 0.05f));
      movement += velocity * (1.0f - decay) / 7.0f;
      velocity *= decay;
      if (std::abs(velocity) < 1) velocity = 0;
    }
    offset = std::clamp(offset - movement, 0.0f, std::max(0.0f, maximum));
    if ((offset <= 0 && velocity > 0) ||
        (offset >= maximum && velocity < 0)) velocity = 0;
    return offset;
  }
};

inline float smoothFlingVelocity(float previous, float sample, float seconds) {
  const float blend = 1.0f - std::exp(-20.0f * std::clamp(seconds, 0.001f, 0.1f));
  return previous + blend * (sample - previous);
}
} // namespace magister

#include "scroll_physics.h"
#include "schedule_gesture.h"
#include <cstdlib>
#include <iostream>

void check(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
float fling(int framesPerSecond) {
  magister::ScrollPhysics scroll{100, 10000};
  float velocity = -1200;
  for (int i = 0; i < framesPerSecond; ++i)
    scroll.advance(0, velocity, false, 1.0f / framesPerSecond);
  return scroll.offset;
}
int main() {
  check(magister::daySwipe(-100, 10, 64) == 1, "Left swipe should advance a day");
  check(magister::daySwipe(100, 10, 64) == -1, "Right swipe should go back a day");
  check(magister::daySwipe(50, 0, 64) == 0, "Short movement is not a swipe");
  check(magister::daySwipe(100, 100, 64) == 0, "Diagonal scrolling is not a swipe");
  check(std::abs(fling(60) - fling(120)) < .01f, "60/120 Hz fling distance differs");
  check(std::abs(fling(90) - fling(120)) < .01f, "90/120 Hz fling distance differs");
  magister::ScrollPhysics scroll{100, 200};
  float velocity = 0;
  check(scroll.advance(25, velocity, true, 1.0f / 60) == 75, "Drag must apply in the same frame");
  velocity = 2000;
  check(scroll.advance(1000, velocity, false, .016f) == 0 && velocity == 0,
        "Top overscroll must clamp and stop");
  velocity = -2000;
  check(scroll.advance(-1000, velocity, false, .016f) == 200 && velocity == 0,
        "Bottom overscroll must clamp and stop");
  check(std::abs(magister::smoothFlingVelocity(0, 2000, .008f)) < 500,
        "A single noisy event should not create a full-speed fling");
  scroll.reset();
  check(scroll.offset == 0 && scroll.maximum == 0, "Page change must reset scroll state");
  std::cout << "Scroll physics checks passed\n";
}

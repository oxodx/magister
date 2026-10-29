#pragma once

#include <fstream>
#include <string>

namespace magister {

// UI preferences intentionally stay small and explicit. They are session-scoped;
// account and authentication data remain owned by the Magister client.
struct AppSettings {
  bool compactSchedule = false;
  bool showHomeworkBadges = true;
  bool reduceMotion = false;
};

inline AppSettings loadSettings(const std::string &path) {
  AppSettings value;
  std::ifstream input(path);
  input >> value.compactSchedule >> value.showHomeworkBadges >> value.reduceMotion;
  return value;
}

inline void saveSettings(const std::string &path, const AppSettings &value) {
  std::ofstream output(path, std::ios::trunc);
  output << value.compactSchedule << ' ' << value.showHomeworkBadges << ' '
         << value.reduceMotion << '\n';
}

} // namespace magister

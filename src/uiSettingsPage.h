#pragma once

void settingsView() {
  secondary("Make the app work the way you expect.");
  gap(14);
  ImGui::SeparatorText("Schedule");
  gap(4);
  bool changed =
      ImGui::Checkbox("Show homework labels", &settings.showHomeworkBadges);
  secondary("Marks lessons that include assigned work.");
  gap(10);
  changed |= ImGui::Checkbox("Compact lesson cards", &settings.compactSchedule);
  secondary("Fits more of your day on screen.");
  gap(18);
  ImGui::SeparatorText("Motion");
  gap(4);
  changed |= ImGui::Checkbox("Reduce motion", &settings.reduceMotion);
  secondary("Stops momentum scrolling after you lift your finger.");
  if (changed)
    magister::saveSettings(settingsPath, settings);
}

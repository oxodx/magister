#pragma once

void moreView(bool busy) {
  beginFeed();
  secondary("SCHOOL"); gap(4);
  const char *titles[] = {"Messages", "Books", "Settings"};
  const char *descriptions[] = {"Your messages and conversations",
    "Textbooks and digital access", "Appearance and schedule preferences"};
  for (int i = 0; i < 3; ++i) {
    ImGui::PushID(i); ImGui::BeginDisabled(busy);
    if (action((std::string(titles[i]) + "\n" + descriptions[i]).c_str(),
               false, ImGui::GetContentRegionAvail().x / uiScale, 76)) {
      if (i == 2) {
        settingsScreen = true; moreScreen = false; resetFeedScroll = true;
      } else {
        switchPage(i == 0 ? Page::Messages : Page::Books);
      }
    }
    ImGui::EndDisabled(); ImGui::PopID();
  }
  gap(20); ImGui::SeparatorText("Account");
  secondary(std::string(state.school.data()) + ".magister.net"); gap(8);
  ImGui::BeginDisabled(busy);
  if (action("Sign out", false, 0, 44)) {
    std::lock_guard lock(state.mutex);
    state.loggedIn = false; state.client.reset(); state.rows = emptyRows();
    state.detailOpen = false; moreScreen = false;
  }
  ImGui::EndDisabled(); ImGui::EndChild();
}

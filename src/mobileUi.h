// Immediate-mode mobile UI: immutable data snapshots and clipped, fixed-height
// lists.
float uiScale = 1.0f;
ImFont *headingFont = nullptr;
ImVec4 ink, muted, blue, surface;
std::array<char, 128> searchText{};
bool moreScreen = false;
bool settingsScreen = false;
bool searchVisible = false;
magister::ScrollPhysics feedScroll;
float dp(float v) { return v * uiScale; }
void gap(float v) { ImGui::Dummy({0, dp(v)}); }
void heading(const std::string &s) {
  ImGui::PushFont(headingFont);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopFont();
}
void secondary(const std::string &s) {
  ImGui::PushStyleColor(ImGuiCol_Text, muted);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
}
std::string plainText(const std::string &html) {
  std::string out;
  bool tag = false;
  for (char c : html) {
    if (c == '<') {
      tag = true;
      if (!out.empty() && out.back() != '\n')
        out += '\n';
    } else if (c == '>')
      tag = false;
    else if (!tag)
      out += c;
  }
  for (const auto &[from, to] :
       std::initializer_list<std::pair<std::string, std::string>>{
           {"&nbsp;", " "},
           {"&amp;", "&"},
           {"&lt;", "<"},
           {"&gt;", ">"},
           {"&quot;", "\""},
           {"&#39;", "'"}}) {
    size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return out;
}
std::string localStamp(std::string s) {
  if (s.size() < 16 || s[10] != 'T')
    return s;
  bool utc = s.find('Z', 16) != std::string::npos;
  auto zone = s.find_first_of("+-", 16);
  if (!utc && zone == std::string::npos)
    return s;
  std::tm tm{};
  if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M", &tm))
    return s;
  auto stamp = timegm(&tm);
  if (zone != std::string::npos && s.size() >= zone + 6) {
    int offset = std::atoi(s.substr(zone + 1, 2).c_str()) * 3600 +
                 std::atoi(s.substr(zone + 4, 2).c_str()) * 60;
    stamp += s[zone] == '+' ? -offset : offset;
  }
  std::tm local{};
  localtime_r(&stamp, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M", &local);
  return text;
}
std::string dateLabel(const std::string &raw) {
  auto s = localStamp(raw);
  if (s.size() < 10)
    return s;
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int m = std::atoi(s.substr(5, 2).c_str());
  return std::to_string(std::atoi(s.substr(8, 2).c_str())) + " " +
         (m >= 1 && m <= 12 ? months[m - 1] : "") + " " + s.substr(0, 4);
}
std::string gradeSubject(const magister::Json &row) {
  auto s = nested(row, "Vak", {"Omschrijving", "Naam"});
  if (s.empty())
    s = value(row, {"VakOmschrijving", "VakNaam", "Vak"});
  return s.empty() ? "Unknown subject" : s;
}
std::string testName(const magister::Json &row) {
  auto s = nested(row, "Toetsinformatie",
                  {"WerkInformatieOmschrijving", "KolomOmschrijving"});
  if (s.empty())
    s = nested(row, "CijferKolom",
               {"KolomOmschrijving", "KolomKop", "KolomNaam"});
  if (s.empty())
    s = value(row, {"Omschrijving", "KolomOmschrijving"});
  return s.empty() ? "Tap for test details" : plainText(s);
}
std::string rowTitle(Page page, const magister::Json &row) {
  if (page == Page::Grades)
    return gradeSubject(row);
  if (page == Page::Schedule) {
    auto subject = firstNested(row, "Vakken", {"Naam", "Omschrijving"});
    return subject.empty() ? value(row, {"Omschrijving", "Titel"}) : subject;
  }
  return value(row, {"Titel", "titel", "Onderwerp", "onderwerp", "Omschrijving",
                     "omschrijving", "Reden"});
}
bool action(const char *label, bool selected = false, float width = 0,
            float height = 42) {
  if (selected)
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
  bool result = ImGui::Button(label, {width ? dp(width) : 0, dp(height)});
  if (selected)
    ImGui::PopStyleColor();
  return result;
}
void changeDay(int day) {
  agendaDay = day;
  {
    std::lock_guard lock(state.mutex);
    state.error.clear();
  }
  resetFeedScroll = true;
  touchScroll.reset();
}
void beginFeed() {
  if (resetFeedScroll) {
    feedScroll.reset();
    touchScroll.reset();
    resetFeedScroll = false;
  } else {
    feedScroll.advance(touchScroll.delta, touchScroll.velocity,
                       touchScroll.down, ImGui::GetIO().DeltaTime);
    touchScroll.delta = 0;
  }
  // Applying this before BeginChild removes SetScrollY's one-frame delay.
  ImGui::SetNextWindowScroll({-1, feedScroll.offset});
  ImGui::BeginChild("feed", {0, -dp(80)}, ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollWithMouse);
  feedScroll.maximum = ImGui::GetScrollMaxY();
  feedScroll.offset = ImGui::GetScrollY();
  touchScroll.min = ImGui::GetWindowPos();
  auto size = ImGui::GetWindowSize();
  touchScroll.max = {touchScroll.min.x + size.x, touchScroll.min.y + size.y};
}
void field(const char *label, const std::string &s) {
  if (s.empty())
    return;
  secondary(label);
  ImGui::TextWrapped("%s", plainText(s).c_str());
  gap(6);
}
void detailsView(Page page, const magister::Json &row, bool busy,
                 const std::string &error) {
  gap(4);
  heading(rowTitle(page, row));
  gap(8);
  beginFeed();
  if (busy)
    secondary("Loading details...");
  if (!error.empty()) {
    ImGui::TextWrapped("%s", error.c_str());
    gap(8);
  }
  if (page == Page::Grades) {
    auto score = value(row, {"CijferStr", "Waarde", "waarde"});
    heading(score);
    field("Test", testName(row));
    auto weight = nested(row, "Toetsinformatie", {"Weging"});
    if (weight.empty())
      weight = value(row, {"Weging"});
    field("Weight", weight.empty() ? "Not available" : weight + "x");
    field("Recorded", dateLabel(value(row, {"DatumIngevoerd", "ingevoerdOp"})));
    field("Teacher", value(row, {"Docent", "Docentcode", "IngevoerdDoor"}));
    field("Period", nested(row, "CijferPeriode", {"Naam", "Omschrijving"}));
    field("Test date",
          dateLabel(nested(row, "Toetsinformatie",
                           {"InvoerDatum", "WerkinformatieDatumIngevoerd"})));
  } else if (page == Page::Books) {
    field("Subject", nested(row, "Vak", {"Omschrijving", "Naam"}));
    field("Publisher", value(row, {"Uitgeverij"}));
    field("ISBN / EAN", value(row, {"EAN"}));
    bool digital = false;
    if (row.contains("Links") && row["Links"].is_array())
      for (const auto &link : row["Links"])
        if (value(link, {"Rel", "rel"}) == "content")
          digital = true;
    if (digital) {
      ImGui::BeginDisabled(busy);
      if (action("Open digital textbook", true, 0, 48))
        openBook(row);
      ImGui::EndDisabled();
      gap(8);
      secondary("Opens your textbook in the browser. The publisher may ask you "
                "to sign in with your school account.");
    } else
      secondary("Physical textbook: no digital access is linked.");
  } else {
    field("Subject", value(row, {"Vak"}));
    auto start = localStamp(value(row, {"Start"})),
         end = localStamp(value(row, {"Einde"}));
    if (!start.empty())
      field("When", dateLabel(start) + "  " +
                        (start.size() >= 16 ? start.substr(11, 5) : "") +
                        (end.size() >= 16 ? " - " + end.substr(11, 5) : ""));
    field("Room", firstNested(row, "Lokalen", {"Naam", "Omschrijving"}));
    field("Teacher", firstNested(row, "Docenten", {"Naam", "Docentcode"}));
    field("Due date", dateLabel(value(row, {"InleverenVoor"})));
    field("Sender", value(row, {"AfzenderNaam", "Verzender"}));
    field("Content",
          value(row, {"Inhoud", "Omschrijving", "Bericht", "inhoud"}));
    field("Assessment", value(row, {"Beoordeling"}));
    field("Submitted", dateLabel(value(row, {"IngeleverdOp"})));
    if (row.contains("Bijlagen") && row["Bijlagen"].is_array()) {
      ImGui::SeparatorText("Attachments");
      for (const auto &attachment : row["Bijlagen"])
        ImGui::TextWrapped(
            "%s", value(attachment, {"Naam", "Titel", "Bestandsnaam"}).c_str());
    }
  }
  ImGui::EndChild();
}
void rowCard(Page page, const magister::Json &row, float height, bool busy) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, surface);
  ImGui::BeginChild("row", {0, height}, ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  auto title = rowTitle(page, row);
  float contentWidth = ImGui::GetContentRegionAvail().x - dp(14);
  if (page == Page::Schedule) {
    auto start = localStamp(value(row, {"Start"})),
         end = localStamp(value(row, {"Einde"}));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(start.size() >= 16 ? start.substr(11, 5).c_str()
                                              : "--:--");
    secondary(end.size() >= 16 ? end.substr(11, 5) : "");
    ImGui::EndGroup();
    ImGui::SameLine(dp(72));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(contentWidth);
    ImGui::TextWrapped("%s", title.c_str());
    auto room = firstNested(row, "Lokalen", {"Naam", "Omschrijving"});
    if (room.empty())
      room = value(row, {"Lokatie", "Locatie"});
    auto teacher = firstNested(row, "Docenten", {"Naam", "Docentcode"});
    secondary(room + (room.empty() || teacher.empty() ? "" : " / ") + teacher);
    auto homework = value(row, {"Inhoud", "Huiswerk", "Aantekening"});
    if (settings.showHomeworkBadges && !plainText(homework).empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, blue);
      ImGui::TextUnformatted("HOMEWORK");
      ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  } else if (page == Page::Grades) {
    auto score = value(row, {"CijferStr", "Waarde", "waarde"});
    auto p = ImGui::GetCursorScreenPos();
    auto positive = row.value("IsVoldoende", true);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(p, {p.x + dp(50), p.y + dp(50)},
                        positive ? IM_COL32(29, 63, 55, 255)
                                 : IM_COL32(73, 36, 44, 255),
                        dp(10));
    auto size = headingFont->CalcTextSizeA(dp(23), 1000, 0, score.c_str());
    draw->AddText(
        headingFont, dp(23),
        {p.x + (dp(50) - size.x) * .5f, p.y + (dp(50) - size.y) * .5f},
        positive ? IM_COL32(115, 211, 179, 255) : IM_COL32(255, 143, 153, 255),
        score.c_str());
    ImGui::Dummy({dp(50), dp(50)});
    ImGui::SameLine(dp(76));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(contentWidth);
    ImGui::TextWrapped("%s", title.c_str());
    secondary(testName(row));
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  } else {
    ImGui::TextWrapped("%s", title.empty() ? "Item" : title.c_str());
    if (page == Page::Assignments) {
      auto subject = value(row, {"Vak"});
      secondary(subject + (subject.empty() ? "" : " / ") + "Due " +
                dateLabel(value(row, {"InleverenVoor"})));
    } else if (page == Page::Books) {
      auto subject = nested(row, "Vak", {"Omschrijving", "Naam"});
      secondary(subject.empty() ? value(row, {"Uitgeverij"}) : subject);
    } else if (page == Page::Messages)
      secondary(value(row, {"AfzenderNaam", "Verzender"}));
    else
      secondary(dateLabel(value(row, {"Start"})));
  }
  auto pos = ImGui::GetWindowPos();
  ImGui::GetWindowDrawList()->AddText(
      {pos.x + ImGui::GetWindowSize().x - dp(17), pos.y + height * .5f - dp(8)},
      IM_COL32(109, 126, 149, 255), ">");
  bool hovered = ImGui::IsWindowHovered();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  if (!busy && !touchScroll.dragging && !touchScroll.swiping && hovered &&
      ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    openDetails(page, row);
}
// Small vector icons avoid font-glyph dependencies on Android.
void navIcon(int type, ImVec2 c, ImU32 color) {
  auto *d = ImGui::GetWindowDrawList();
  float s = dp(10), t = dp(1.6f);
  if (type == 0) {
    d->AddRect({c.x - s, c.y - s * .8f}, {c.x + s, c.y + s}, color, dp(2), 0,
               t);
    d->AddLine({c.x - s, c.y - s * .25f}, {c.x + s, c.y - s * .25f}, color, t);
    for (int x = -1; x <= 1; ++x)
      d->AddCircleFilled({c.x + x * s * .5f, c.y + s * .4f}, dp(1.1f), color);
    for (int x = -1; x <= 1; x += 2)
      d->AddLine({c.x + x * s * .5f, c.y - s},
                 {c.x + x * s * .5f, c.y - s * .5f}, color, t);
  } else if (type == 1) {
    for (int x = 0; x < 3; ++x)
      d->AddRectFilled({c.x - s + x * s * .75f, c.y + s - (x + 1) * s * .6f},
                       {c.x - s + x * s * .75f + s * .45f, c.y + s}, color,
                       dp(1));
  } else if (type == 2) {
    d->AddRect({c.x - s, c.y - s}, {c.x + s, c.y + s}, color, dp(3), 0, t);
    d->AddLine({c.x - s * .55f, c.y}, {c.x - s * .1f, c.y + s * .45f}, color,
               t);
    d->AddLine({c.x - s * .1f, c.y + s * .45f},
               {c.x + s * .65f, c.y - s * .45f}, color, t);
  } else if (type == 3) {
    d->AddCircle(c, s, color, 0, t);
    d->AddLine(c, {c.x, c.y - s * .55f}, color, t);
    d->AddLine(c, {c.x + s * .45f, c.y + s * .25f}, color, t);
  } else {
    for (int y = -1; y <= 1; y += 2)
      for (int x = -1; x <= 1; x += 2)
        d->AddRect({c.x + x * s * .5f - dp(3), c.y + y * s * .5f - dp(3)},
                   {c.x + x * s * .5f + dp(3), c.y + y * s * .5f + dp(3)},
                   color, dp(1), 0, t);
  }
}
bool bottomItem(int index, const char *label, bool selected, float width) {
  ImGui::PushID(index);
  auto p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton("tab", {width, dp(62)});
  auto *d = ImGui::GetWindowDrawList();
  auto color = ImGui::GetColorU32(selected ? blue : muted);
  if (selected)
    d->AddRectFilled({p.x + width * .5f - dp(23), p.y + dp(3)},
                     {p.x + width * .5f + dp(23), p.y + dp(33)},
                     IM_COL32(40, 66, 99, 255), dp(14));
  navIcon(index, {p.x + width * .5f, p.y + dp(18)}, color);
  auto *font = ImGui::GetIO().Fonts->Fonts[0];
  auto size = font->CalcTextSizeA(dp(12), 1000, 0, label);
  d->AddText(font, dp(12), {p.x + (width - size.x) * .5f, p.y + dp(40)}, color,
             label);
  ImGui::PopID();
  return pressed;
}
void switchPage(Page page) {
  moreScreen = false;
  settingsScreen = false;
  searchVisible = false;
  searchText.fill(0);
  platform::hideKeyboard();
  load(page);
}
void weekPicker() {
  auto date = dateOffset(agendaDay);
  float available = ImGui::GetContentRegionAvail().x;
  if (action("<", false, 42, 40))
    changeDay(agendaDay - 1);
  ImGui::SameLine();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(dateLabel(date).c_str());
  ImGui::SameLine();
  ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - dp(42));
  if (action(">", false, 42, 40))
    changeDay(agendaDay + 1);
  std::tm tm{};
  strptime(date.c_str(), "%Y-%m-%d", &tm);
  tm.tm_hour = 12;
  tm.tm_isdst = -1;
  std::mktime(&tm);
  int base = agendaDay - (tm.tm_wday + 6) % 7;
  const char *days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  float width = (available - dp(24)) / 7;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {dp(4), dp(8)});
  for (int i = 0; i < 7; ++i) {
    if (i)
      ImGui::SameLine();
    ImGui::PushID(i);
    auto day = dateOffset(base + i);
    auto label = std::string(days[i]) + "\n" +
                 std::to_string(std::atoi(day.substr(8, 2).c_str()));
    ImGui::PushStyleColor(ImGuiCol_Button,
                          base + i == agendaDay ? blue : ImVec4(0, 0, 0, 0));
    if (ImGui::Button(label.c_str(), {width, dp(52)}))
      changeDay(base + i);
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::PopStyleVar();
}
#include "uiMorePage.h"
#include "uiSettingsPage.h"
void mobileUi() {
  bool logged, busy, detailOpen;
  Page page;
  std::string status, error;
  Rows snapshot, detail;
  int loadedDay;
  {
    std::lock_guard lock(state.mutex);
    logged = state.loggedIn;
    busy = state.busy;
    detailOpen = state.detailOpen;
    page = state.page;
    status = state.status;
    error = state.error;
    snapshot = state.rows;
    detail = state.detail;
    loadedDay = state.loadedDay;
  }
  const auto &rows = *snapshot;
  if (requestedDayChange) {
    if (logged && !moreScreen && !settingsScreen && page == Page::Schedule &&
        !detailOpen)
      changeDay(agendaDay + requestedDayChange);
    requestedDayChange = 0;
  }
  if (logged && !moreScreen && !settingsScreen && page == Page::Schedule &&
      !detailOpen && !busy && error.empty() && loadedDay != agendaDay)
    load(Page::Schedule);
  const char *names[] = {"Schedule", "Grades",   "Tasks",
                         "Books",    "Messages", "Attendance"};
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings;
  if (logged)
    flags |= ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
  ImGui::Begin("Magister", nullptr, flags);
  if (!logged) {
    gap(24);
    ImGui::TextColored(blue, "MAGISTER");
    gap(18);
    heading("Ready for your school day.");
    gap(6);
    secondary("Your schedule, grades and textbooks in one place.");
    gap(20);
    ImGui::SeparatorText("Welcome back");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##school", "Your school, for example svol",
                             state.school.data(), state.school.size());
    requestKeyboard |= ImGui::IsItemActivated();
    gap(6);
    ImGui::BeginDisabled(busy);
    if (action(busy ? "Please wait..." : "Continue with my school", true,
               ImGui::GetContentRegionAvail().x / uiScale, 48)) {
      platform::hideKeyboard();
      login();
    }
    ImGui::EndDisabled();
    gap(10);
    secondary("Sign in securely through Magister.");
    if (!error.empty())
      secondary(error);
  } else {
    // A single header and overflow menu replace the old button toolbar.
    float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::BeginGroup();
    if (detailOpen && !moreScreen) {
      if (action("< Back", false, 82, 44)) {
        std::lock_guard lock(state.mutex);
        state.detailOpen = false;
        resetFeedScroll = true;
        touchScroll.reset();
      }
    } else {
      heading(settingsScreen
                  ? "Settings"
                  : (moreScreen ? "More" : names[static_cast<int>(page)]));
      secondary(moreScreen ? "Everything else, neatly organised"
                           : std::string(state.school.data()) + " / Magister");
    }
    ImGui::EndGroup();
    if (!moreScreen && !settingsScreen && !detailOpen) {
      ImGui::SameLine();
      ImGui::SetCursorPosX(right - dp(44));
      if (action("...", false, 44, 44))
        ImGui::OpenPopup("options");
      if (ImGui::BeginPopup("options")) {
        ImGui::BeginDisabled(busy);
        if (ImGui::Selectable("Refresh", false, 0, {dp(180), dp(38)})) {
          load(page);
          ImGui::CloseCurrentPopup();
        }
        if (page == Page::Schedule) {
          if (ImGui::Selectable("Go to today", false, 0, {dp(180), dp(38)})) {
            changeDay(0);
            ImGui::CloseCurrentPopup();
          }
        } else if (ImGui::Selectable(searchVisible ? "Close search" : "Search",
                                     false, 0, {dp(180), dp(38)})) {
          searchVisible = !searchVisible;
          searchText.fill(0);
          platform::hideKeyboard();
          ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
      }
    }
    gap(4);
    if (busy)
      ImGui::ProgressBar(-static_cast<float>(ImGui::GetTime()), {-1, dp(2)},
                         "");
    else
      ImGui::Dummy({0, dp(2)});
    gap(4);
    if (settingsScreen)
      settingsView();
    else if (moreScreen)
      moreView(busy);
    else if (detailOpen)
      detailsView(page, *detail, busy, error);
    else {
      if (page == Page::Schedule) {
        weekPicker();
        gap(4);
      } else if (searchVisible) {
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##search", "Search by subject or title",
                                     searchText.data(), searchText.size()))
          resetFeedScroll = true;
        requestKeyboard |= ImGui::IsItemActivated();
        gap(4);
      } else {
        const char *subtitles[] = {"",
                                   "Your latest results",
                                   "Upcoming assignments and deadlines",
                                   "All your textbooks in one place",
                                   "Your school messages",
                                   "Your attendance records"};
        secondary(subtitles[static_cast<int>(page)]);
        gap(4);
      }
      beginFeed();
      if (!error.empty()) {
        ImGui::TextColored({1, .5f, .5f, 1}, "Unable to load this page");
        secondary(error);
        if (!busy && action("Try again"))
          load(page);
      }
      std::vector<size_t> visible;
      const auto selectedDate = dateOffset(agendaDay);
      std::string query = searchText.data();
      std::transform(query.begin(), query.end(), query.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      for (size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        if (page == Page::Schedule &&
            localStamp(value(row, {"Start"})).substr(0, 10) != selectedDate)
          continue;
        if (page != Page::Schedule && !query.empty()) {
          auto text = rowTitle(page, row) + " " + testName(row);
          std::transform(text.begin(), text.end(), text.begin(),
                         [](unsigned char c) { return std::tolower(c); });
          if (text.find(query) == std::string::npos)
            continue;
        }
        visible.push_back(i);
      }
      if (!busy && error.empty() && visible.empty()) {
        gap(24);
        heading(page == Page::Schedule ? "Nothing scheduled"
                                       : "You're all caught up");
        secondary(
            page == Page::Schedule
                ? "Choose another day in the week above."
                : "New items will appear here when your school adds them.");
      }
      const float height = dp(
          page == Page::Books
              ? 110
              : (page == Page::Schedule && settings.compactSchedule ? 80 : 88));
      ImGuiListClipper clipper;
      clipper.Begin(static_cast<int>(visible.size()),
                    height + ImGui::GetStyle().ItemSpacing.y);
      while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
          ImGui::PushID(static_cast<int>(visible[i]));
          rowCard(page, rows[visible[i]], height, busy);
          ImGui::PopID();
        }
      ImGui::EndChild();
    }
    // Anchor the bar instead of letting accumulated layout spacing push it
    // down.
    ImGui::SetCursorPosY(ImGui::GetIO().DisplaySize.y -
                         ImGui::GetStyle().WindowPadding.y - dp(64));
    auto navPos = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddLine(
        {navPos.x, navPos.y - dp(6)},
        {navPos.x + ImGui::GetContentRegionAvail().x, navPos.y - dp(6)},
        IM_COL32(47, 55, 69, 255));
    float width = (ImGui::GetContentRegionAvail().x - dp(16)) / 5;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {dp(4), 0});
    const char *tabs[] = {"Schedule", "Grades", "Tasks", "Attendance", "More"};
    int active = moreScreen || settingsScreen || page == Page::Books ||
                         page == Page::Messages
                     ? 4
                     : (page == Page::Absences ? 3 : static_cast<int>(page));
    ImGui::BeginDisabled(busy);
    for (int i = 0; i < 5; ++i) {
      if (i)
        ImGui::SameLine();
      if (bottomItem(i, tabs[i], active == i, width)) {
        if (i == 4) {
          moreScreen = true;
          settingsScreen = false;
          {
            std::lock_guard lock(state.mutex);
            state.detailOpen = false;
          }
          platform::hideKeyboard();
          resetFeedScroll = true;
          touchScroll.reset();
        } else {
          Page target = i == 0   ? Page::Schedule
                        : i == 1 ? Page::Grades
                        : i == 2 ? Page::Assignments
                                 : Page::Absences;
          if (moreScreen || settingsScreen || page != target || detailOpen)
            switchPage(target);
        }
      }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
  }
  ImGui::End();
}

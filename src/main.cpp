#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "magister_client.h"
#include "app_settings.h"
#include "schedule_gesture.h"
#include "scroll_physics.h"

#include <GLES3/gl3.h>
#include <SDL.h>
#include <SDL_system.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <android/native_window.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <dlfcn.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
SDL_Window *window = nullptr;
SDL_GLContext glContext = nullptr;
std::string iniPath;
std::string settingsPath;
bool requestKeyboard = false;
int agendaDay = 0;
int requestedDayChange = 0;

enum class Page { Schedule, Grades, Assignments, Books, Messages, Absences };

using Rows = std::shared_ptr<const magister::Json>;

Rows emptyRows() {
  return std::make_shared<const magister::Json>(magister::Json::array());
}

struct TouchScroll {
  ImVec2 min{}, max{};
  bool down = false, eligible = false, dragging = false, swiping = false;
  float start = 0, last = 0, startX = 0, lastX = 0;
  float delta = 0, velocity = 0, verticalTravel = 0;
  Uint32 timestamp = 0;
  void reset() {
    delta = velocity = verticalTravel = 0;
    eligible = dragging = swiping = false;
  }
} touchScroll;

magister::AppSettings settings;

bool resetFeedScroll = true;

struct State {
  std::array<char, 128> school{};
  bool loggedIn = false, busy = false;
  std::string status = "Enter your Magister account";
  std::string error;
  Page page = Page::Schedule;
  int loadedDay = 0;
  Rows rows = emptyRows();
  Rows detail =
      std::make_shared<const magister::Json>(magister::Json::object());
  bool detailOpen = false;
  std::shared_ptr<magister::Client> client;
  std::mutex mutex;
} state;

std::string dateOffset(int days) {
  auto now = std::chrono::system_clock::now();
  std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
  localtime_r(&raw, &local);
  local.tm_hour = 12;
  local.tm_mday += days;
  local.tm_isdst = -1;
  std::mktime(&local);
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
  return text;
}

std::string value(const magister::Json &item,
                  std::initializer_list<const char *> keys) {
  for (const char *key : keys) {
    if (item.contains(key) && !item[key].is_null()) {
      if (item[key].is_string())
        return item[key].get<std::string>();
      if (item[key].is_number())
        return item[key].dump();
      if (item[key].is_boolean())
        return item[key].get<bool>() ? "Yes" : "No";
    }
  }
  return {};
}

std::string nested(const magister::Json &item, const char *object,
                   std::initializer_list<const char *> keys) {
  if (!item.contains(object) || !item[object].is_object())
    return {};
  return value(item[object], keys);
}

std::string firstNested(const magister::Json &item, const char *array,
                        std::initializer_list<const char *> keys) {
  if (!item.contains(array) || !item[array].is_array() || item[array].empty())
    return {};
  return value(item[array][0], keys);
}

void runAsync(std::function<void()> task) {
  {
    std::lock_guard lock(state.mutex);
    if (state.busy)
      return;
    state.busy = true;
    state.error.clear();
    state.status = "Loading...";
  }
  std::thread([task = std::move(task)] {
    try {
      task();
    } catch (const std::exception &error) {
      std::lock_guard lock(state.mutex);
      state.status = error.what();
      state.error = error.what();
    }
    std::lock_guard lock(state.mutex);
    state.busy = false;
  }).detach();
}

void load(Page page) {
  {
    std::lock_guard lock(state.mutex);
    if (state.busy)
      return;
    state.page = page;
    state.detailOpen = false;
    state.rows = emptyRows();
  }
  resetFeedScroll = true;
  touchScroll.reset();
  int day = agendaDay;
  runAsync([page, day] {
    std::shared_ptr<magister::Client> client;
    {
      std::lock_guard lock(state.mutex);
      client = state.client;
    }
    magister::Json data;
    switch (page) {
    case Page::Schedule:
      data = client->appointments(dateOffset(day - 3), dateOffset(day + 4));
      break;
    case Page::Grades:
      data = client->grades();
      break;
    case Page::Assignments:
      data = client->assignments(dateOffset(-90), dateOffset(90));
      break;
    case Page::Books:
      data = client->books();
      break;
    case Page::Messages:
      data = client->messages();
      break;
    case Page::Absences:
      data = client->absences(dateOffset(-30), dateOffset(0));
      break;
    }
    if (page == Page::Schedule)
      std::stable_sort(data.begin(), data.end(),
                       [](const auto &a, const auto &b) {
                         return value(a, {"Start"}) < value(b, {"Start"});
                       });
    std::lock_guard lock(state.mutex);
    state.page = page;
    state.rows = std::make_shared<const magister::Json>(std::move(data));
    if (page == Page::Schedule)
      state.loadedDay = day;
    state.status = state.rows->empty()
                       ? "No results"
                       : std::to_string(state.rows->size()) + " results";
  });
}

void login() {
  std::string school = state.school.data();
  runAsync([school, day = agendaDay] {
    if (school.empty())
      throw std::runtime_error("School is required");
    auto client =
        std::make_shared<magister::Client>(magister::AndroidHttp(), school);
    client->login();
    auto data = client->appointments(dateOffset(day - 3), dateOffset(day + 4));
    std::lock_guard lock(state.mutex);
    state.client = std::move(client);
    state.loggedIn = true;
    state.page = Page::Schedule;
    state.loadedDay = day;
    std::stable_sort(data.begin(), data.end(),
                     [](const auto &a, const auto &b) {
                       return value(a, {"Start"}) < value(b, {"Start"});
                     });
    state.rows = std::make_shared<const magister::Json>(std::move(data));
    state.status = "Signed in";
  });
}

void openBook(magister::Json book) {
  runAsync([book = std::move(book)] {
    std::shared_ptr<magister::Client> client;
    {
      std::lock_guard lock(state.mutex);
      client = state.client;
    }
    if (!client)
      throw std::runtime_error("Sign in again to open your textbook.");
    auto url = client->bookUrl(book);
    if (SDL_OpenURL(url.c_str()) != 0)
      throw std::runtime_error("Unable to open the browser.");
    std::lock_guard lock(state.mutex);
    state.status = "Textbook opened in the browser";
  });
}

void openDetails(Page page, magister::Json row) {
  {
    std::lock_guard lock(state.mutex);
    if (state.busy)
      return;
    state.detailOpen = true;
    state.detail = std::make_shared<const magister::Json>(row);
  }
  touchScroll.reset();
  resetFeedScroll = true;
  runAsync([page, row = std::move(row)] {
    std::shared_ptr<magister::Client> client;
    {
      std::lock_guard lock(state.mutex);
      client = state.client;
    }
    const char *kinds[] = {"schedule", "grades",   "assignments",
                           "books",    "messages", "absences"};
    auto data = client->details(kinds[static_cast<int>(page)], row);
    std::lock_guard lock(state.mutex);
    state.detail = std::make_shared<const magister::Json>(std::move(data));
  });
}

#include "mobile_ui.h"
} // namespace

int main(int, char **) {
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
  SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
  SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1");
  SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    return 1;
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
  window = SDL_CreateWindow(
      "Magister", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 0, 0,
      SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
  if (!window) {
    SDL_Quit();
    return 2;
  }
  glContext = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, glContext);
  SDL_GL_SetSwapInterval(1);
  // Android 11+ supports an NDK frame-rate hint. Resolve dynamically so the
  // same APK still runs on API 24; Android retains power/thermal control.
  SDL_SysWMinfo platformWindow{};
  SDL_VERSION(&platformWindow.version);
  using FrameRateHint = int32_t (*)(ANativeWindow *, float, int8_t);
  auto frameRateHint = reinterpret_cast<FrameRateHint>(
      dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
  if (frameRateHint && SDL_GetWindowWMInfo(window, &platformWindow) &&
      platformWindow.subsystem == SDL_SYSWM_ANDROID) {
    frameRateHint(platformWindow.info.android.window, 120.0f, 0);
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  iniPath = std::string(SDL_AndroidGetInternalStoragePath()) + "/imgui.ini";
  settingsPath = std::string(SDL_AndroidGetInternalStoragePath()) + "/settings.txt";
  settings = magister::loadSettings(settingsPath);
  io.IniFilename = iniPath.c_str();
  int screenWidth = 0, screenHeight = 0;
  SDL_GetWindowSize(window, &screenWidth, &screenHeight);
  float scale = std::max(1.0f, screenWidth / 400.0f);
  uiScale = scale;
  ImFontConfig font;
  font.SizePixels = 16.0f * scale;
  // Use a scalable font on high-density displays; keep ImGui's own theme.
  if (access("/system/fonts/Roboto-Regular.ttf", R_OK) == 0)
    io.Fonts->AddFontFromFileTTF("/system/fonts/Roboto-Regular.ttf",
                                 font.SizePixels, &font);
  else
    io.Fonts->AddFontDefault(&font);
  headingFont = io.Fonts->Fonts[0];
  if (access("/system/fonts/Roboto-Regular.ttf", R_OK) == 0)
    headingFont = io.Fonts->AddFontFromFileTTF(
        "/system/fonts/Roboto-Regular.ttf", 23 * scale);
  ImGui::StyleColorsDark();
  ImGuiStyle &style = ImGui::GetStyle();
  // Keep the official palette and geometry, scaling once for the display.
  style.FramePadding = {8, 8};
  style.WindowPadding = {12, 12};
  style.ItemSpacing = {8, 8};
  style.TouchExtraPadding = {2, 2};
  style.ChildRounding = 10;
  style.FrameRounding = 8;
  style.ScaleAllSizes(scale);
  ink = style.Colors[ImGuiCol_Text];
  muted = style.Colors[ImGuiCol_TextDisabled];
  blue = style.Colors[ImGuiCol_HeaderActive];
  surface = {.095f, .11f, .14f, 1};
  style.Colors[ImGuiCol_WindowBg] = {.055f, .065f, .085f, 1};
  style.Colors[ImGuiCol_Button] = {.12f, .14f, .18f, 1};
  style.Colors[ImGuiCol_ButtonHovered] = {.18f, .22f, .28f, 1};
  style.Colors[ImGuiCol_ButtonActive] = {.20f, .30f, .43f, 1};
  style.Colors[ImGuiCol_FrameBg] = {.10f, .12f, .16f, 1};
  ImGui_ImplSDL2_InitForOpenGL(window, glContext);
  ImGui_ImplOpenGL3_Init("#version 300 es");
  bool running = true;
  bool keyboardVisible = false;
  ImVec2 fingerPos{};
  bool fingerMoved = false;
  std::vector<double> scrollFrameTimes;
  while (running) {
    const auto frameStart = SDL_GetPerformanceCounter();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      if (event.type == SDL_FINGERDOWN || event.type == SDL_FINGERMOTION ||
          event.type == SDL_FINGERUP) {
        int logicalWidth = 0, logicalHeight = 0;
        SDL_GetWindowSize(window, &logicalWidth, &logicalHeight);
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        fingerPos = {event.tfinger.x * logicalWidth,
                     event.tfinger.y * logicalHeight};
        if (event.type == SDL_FINGERDOWN) {
          touchScroll.reset();
          touchScroll.down = true;
          touchScroll.start = touchScroll.last = fingerPos.y;
          touchScroll.startX = touchScroll.lastX = fingerPos.x;
          touchScroll.timestamp = event.tfinger.timestamp;
          touchScroll.eligible = fingerPos.x >= touchScroll.min.x &&
                                 fingerPos.x <= touchScroll.max.x &&
                                 fingerPos.y >= touchScroll.min.y &&
                                 fingerPos.y <= touchScroll.max.y;
          io.AddMousePosEvent(fingerPos.x, fingerPos.y);
          io.AddMouseButtonEvent(0, true);
        } else if (event.type == SDL_FINGERMOTION) {
          fingerMoved = true;
          float y = fingerPos.y;
          const float dx = fingerPos.x - touchScroll.startX;
          const float dy = y - touchScroll.start;
          if (touchScroll.eligible && !touchScroll.dragging &&
              !touchScroll.swiping && std::abs(dx) > 12 * uiScale &&
              std::abs(dx) > std::abs(dy) * 1.25f) {
            touchScroll.swiping = true;
            io.AddMousePosEvent(-10000, -10000);
            io.AddMouseButtonEvent(0, false);
          } else if (touchScroll.eligible && !touchScroll.dragging &&
                     !touchScroll.swiping && std::abs(dy) > 7 * uiScale &&
                     std::abs(dy) > std::abs(dx) * 0.8f) {
            touchScroll.dragging = true;
            io.AddMousePosEvent(-10000, -10000);
            io.AddMouseButtonEvent(0, false);
          }
          if (touchScroll.dragging) {
            float distance = y - touchScroll.last;
            touchScroll.verticalTravel += std::abs(distance);
            float seconds = std::max(
                0.001f,
                (event.tfinger.timestamp - touchScroll.timestamp) / 1000.0f);
            touchScroll.delta += distance;
            touchScroll.velocity = magister::smoothFlingVelocity(
                touchScroll.velocity,
                std::clamp(distance / seconds, -5000 * uiScale, 5000 * uiScale),
                seconds);
          }
          touchScroll.last = y;
          touchScroll.lastX = fingerPos.x;
          touchScroll.timestamp = event.tfinger.timestamp;
        } else if (event.type == SDL_FINGERUP) {
          if (touchScroll.eligible && touchScroll.swiping) {
            requestedDayChange = magister::daySwipe(
                fingerPos.x - touchScroll.startX,
                fingerPos.y - touchScroll.start, 64 * uiScale);
          }
          if (!touchScroll.dragging && !touchScroll.swiping)
            io.AddMousePosEvent(fingerPos.x, fingerPos.y);
          io.AddMouseButtonEvent(0, false);
          touchScroll.down = false;
          if (event.tfinger.timestamp - touchScroll.timestamp > 100)
            touchScroll.velocity = 0;
        }
      }
      if (event.type == SDL_QUIT)
        running = false;
    }
    if (fingerMoved && !touchScroll.dragging && !touchScroll.swiping)
      io.AddMousePosEvent(fingerPos.x, fingerPos.y);
    fingerMoved = false;
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (settings.reduceMotion && !touchScroll.down)
      touchScroll.velocity = 0;
    mobileUi();
    ImGui::Render();
    if (requestKeyboard) {
      // Android keyboards keep a composing/autocorrect span in SDL's hidden
      // EditText. Reset that span when switching fields, then keep text input
      // alive; stopping it whenever WantTextInput flickers loses composition.
      if (keyboardVisible)
        SDL_StopTextInput();
      SDL_StartTextInput();
      keyboardVisible = true;
      requestKeyboard = false;
    }
    int width, height;
    SDL_GL_GetDrawableSize(window, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(.035f, .055f, .10f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
    const bool scrolling = (touchScroll.down && touchScroll.dragging) ||
                           std::abs(touchScroll.velocity) > 10 * uiScale;
    if (scrolling && scrollFrameTimes.size() < 1024) {
      scrollFrameTimes.push_back(1000.0 *
                                 (SDL_GetPerformanceCounter() - frameStart) /
                                 SDL_GetPerformanceFrequency());
    } else if (!scrolling && !scrollFrameTimes.empty()) {
      std::sort(scrollFrameTimes.begin(), scrollFrameTimes.end());
      double total = 0;
      for (double ms : scrollFrameTimes)
        total += ms;
      SDL_Log("Scroll frames: %zu, mean %.2f ms, p95 %.2f ms",
              scrollFrameTimes.size(), total / scrollFrameTimes.size(),
              scrollFrameTimes[scrollFrameTimes.size() * 95 / 100]);
      scrollFrameTimes.clear();
    }
  }
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();
  SDL_GL_DeleteContext(glContext);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

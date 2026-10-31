#include "androidPlatform.h"
#include "appSettings.h"
#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"
#include "magisterClient.h"
#include "scheduleGesture.h"
#include "scrollPhysics.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
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
android_app *gApp = nullptr;
EGLDisplay eglDisplay = EGL_NO_DISPLAY;
EGLSurface eglSurface = EGL_NO_SURFACE;
EGLContext eglContext = EGL_NO_CONTEXT;
bool windowReady = false;
bool imguiInitialized = false;
bool imguiBackends = false;
bool keyboardVisible = false;
int windowWidth = 0;
int windowHeight = 0;
std::vector<double> scrollFrameTimes;
std::string iniPath;
std::string settingsPath;
bool requestKeyboard = false;
int agendaDay = 0;
int requestedDayChange = 0;
bool sessionRestoreStarted = false;

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
  std::int64_t timestamp = 0;
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
        std::make_shared<magister::Client>(magister::HttpClient(), school);
    client->login();
    platform::saveSession(magister::Client::normalizeSchool(school),
                          client->accessToken());
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

void restoreSavedSession() {
  if (sessionRestoreStarted)
    return;
  sessionRestoreStarted = true;
  std::string school = magister::Client::normalizeSchool(platform::loadSchool());
  if (!school.empty()) {
    state.school.fill(0);
    std::copy_n(school.data(),
                std::min(school.size(), state.school.size() - 1),
                state.school.data());
  }
  std::string token = platform::loadSessionToken();
  if (school.empty() || token.empty())
    return;
  runAsync([school = std::move(school), token = std::move(token),
            day = agendaDay] {
    auto client =
        std::make_shared<magister::Client>(magister::HttpClient(), school);
    client->restoreSession(token);
    magister::Json data;
    try {
      data = client->appointments(dateOffset(day - 3), dateOffset(day + 4));
    } catch (const std::exception &error) {
      const std::string message = error.what();
      if (message.find("HTTP 401") == std::string::npos &&
          message.find("Session expired") == std::string::npos)
        throw;
      // The official WebView retains Magister's own login cookie. In the
      // common case this refreshes silently; school SSO may still ask the user.
      client->login();
      platform::saveSession(school, client->accessToken());
      data = client->appointments(dateOffset(day - 3), dateOffset(day + 4));
    }
    std::stable_sort(data.begin(), data.end(),
                     [](const auto &a, const auto &b) {
                       return value(a, {"Start"}) < value(b, {"Start"});
                     });
    std::lock_guard lock(state.mutex);
    state.client = std::move(client);
    state.loggedIn = true;
    state.page = Page::Schedule;
    state.loadedDay = day;
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
    platform::openUrl(url);
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

#include "mobileUi.h"
} // namespace

void updateWindowSize() {
  if (!gApp || !gApp->window)
    return;
  windowWidth = ANativeWindow_getWidth(gApp->window);
  windowHeight = ANativeWindow_getHeight(gApp->window);
}

void requestFrameRate(ANativeWindow *window) {
  // Android 11+ supports an NDK frame-rate hint. Resolve dynamically so the
  // same APK still runs on API 24; Android retains power/thermal control.
  using FrameRateHint = int32_t (*)(ANativeWindow *, float, int8_t);
  auto frameRateHint = reinterpret_cast<FrameRateHint>(
      dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate"));
  if (frameRateHint)
    frameRateHint(window, 120.0f, 0);
}

void shutdownEgl() {
  if (eglDisplay != EGL_NO_DISPLAY) {
    eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglSurface != EGL_NO_SURFACE)
      eglDestroySurface(eglDisplay, eglSurface);
    if (eglContext != EGL_NO_CONTEXT)
      eglDestroyContext(eglDisplay, eglContext);
    eglTerminate(eglDisplay);
  }
  eglDisplay = EGL_NO_DISPLAY;
  eglSurface = EGL_NO_SURFACE;
  eglContext = EGL_NO_CONTEXT;
}

bool initEgl(android_app *app) {
  eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (eglDisplay == EGL_NO_DISPLAY)
    return false;
  EGLint major = 0, minor = 0;
  if (!eglInitialize(eglDisplay, &major, &minor)) {
    shutdownEgl();
    return false;
  }
  const EGLint configAttributes[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE,
      EGL_WINDOW_BIT,      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,                   EGL_BLUE_SIZE,
      8,                   EGL_ALPHA_SIZE,      8,
      EGL_NONE};
  EGLConfig config = nullptr;
  EGLint configCount = 0;
  if (!eglChooseConfig(eglDisplay, configAttributes, &config, 1,
                       &configCount) ||
      configCount < 1) {
    shutdownEgl();
    return false;
  }
  const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  eglContext =
      eglCreateContext(eglDisplay, config, EGL_NO_CONTEXT, contextAttributes);
  if (eglContext == EGL_NO_CONTEXT) {
    shutdownEgl();
    return false;
  }
  EGLint format = 0;
  eglGetConfigAttrib(eglDisplay, config, EGL_NATIVE_VISUAL_ID, &format);
  ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);
  eglSurface = eglCreateWindowSurface(eglDisplay, config, app->window, nullptr);
  if (eglSurface == EGL_NO_SURFACE ||
      !eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
    shutdownEgl();
    return false;
  }
  eglSwapInterval(eglDisplay, 1);
  return true;
}

void initImGuiBackends(android_app *app) {
  ImGui_ImplAndroid_Init(app->window);
  ImGui_ImplOpenGL3_Init("#version 300 es");
  imguiBackends = true;
}

void initializeImGui(android_app *app) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  imguiInitialized = true;
  ImGuiIO &io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  iniPath = platform::internalDataPath() + "/imgui.ini";
  settingsPath = platform::internalDataPath() + "/settings.txt";
  settings = magister::loadSettings(settingsPath);
  io.IniFilename = iniPath.c_str();
  updateWindowSize();
  float scale = std::max(1.0f, windowWidth / 400.0f);
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
  initImGuiBackends(app);
  restoreSavedSession();
}

void handleAppCommand(android_app *app, int32_t command) {
  switch (command) {
  case APP_CMD_INIT_WINDOW:
    if (!app->window)
      break;
    if (!initEgl(app)) {
      platform::log("Unable to initialize EGL");
      break;
    }
    updateWindowSize();
    requestFrameRate(app->window);
    if (!imguiInitialized)
      initializeImGui(app);
    else if (!imguiBackends)
      initImGuiBackends(app);
    windowReady = true;
    break;
  case APP_CMD_TERM_WINDOW:
    windowReady = false;
    if (imguiBackends) {
      ImGui_ImplOpenGL3_Shutdown();
      ImGui_ImplAndroid_Shutdown();
      imguiBackends = false;
    }
    shutdownEgl();
    break;
  case APP_CMD_WINDOW_RESIZED:
  case APP_CMD_CONFIG_CHANGED:
    updateWindowSize();
    break;
  case APP_CMD_LOST_FOCUS:
    touchScroll.reset();
    touchScroll.down = false;
    break;
  default:
    break;
  }
}

int32_t handleInput(android_app *, AInputEvent *event) {
  const int32_t type = AInputEvent_getType(event);
  if (type == AINPUT_EVENT_TYPE_KEY) {
    const int32_t keyCode = AKeyEvent_getKeyCode(event);
    const int32_t action = AKeyEvent_getAction(event);
    if (keyCode == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_UP) {
      platform::finish();
      return 1;
    }
    ImGui_ImplAndroid_HandleInputEvent(event);
    return 1;
  }
  if (type != AINPUT_EVENT_TYPE_MOTION)
    return 0;

  ImGui_ImplAndroid_HandleInputEvent(event);
  const int32_t action =
      AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  const float x = AMotionEvent_getX(event, 0);
  const float y = AMotionEvent_getY(event, 0);
  const std::int64_t timestamp = AMotionEvent_getEventTime(event) / 1000000;
  if (action == AMOTION_EVENT_ACTION_DOWN) {
    touchScroll.reset();
    touchScroll.down = true;
    touchScroll.start = touchScroll.last = y;
    touchScroll.startX = touchScroll.lastX = x;
    touchScroll.timestamp = timestamp;
    touchScroll.eligible = x >= touchScroll.min.x && x <= touchScroll.max.x &&
                           y >= touchScroll.min.y && y <= touchScroll.max.y;
  } else if (action == AMOTION_EVENT_ACTION_MOVE && touchScroll.down) {
    const float dx = x - touchScroll.startX;
    const float dy = y - touchScroll.start;
    if (touchScroll.eligible && !touchScroll.dragging &&
        !touchScroll.swiping && std::abs(dx) > 12 * uiScale &&
        std::abs(dx) > std::abs(dy) * 1.25f) {
      touchScroll.swiping = true;
      ImGui::GetIO().AddMousePosEvent(-10000, -10000);
      ImGui::GetIO().AddMouseButtonEvent(0, false);
    } else if (touchScroll.eligible && !touchScroll.dragging &&
               !touchScroll.swiping && std::abs(dy) > 7 * uiScale &&
               std::abs(dy) > std::abs(dx) * 0.8f) {
      touchScroll.dragging = true;
      ImGui::GetIO().AddMousePosEvent(-10000, -10000);
      ImGui::GetIO().AddMouseButtonEvent(0, false);
    }
    if (touchScroll.dragging) {
      const float distance = y - touchScroll.last;
      touchScroll.verticalTravel += std::abs(distance);
      const float seconds = std::max(
          0.001f, (timestamp - touchScroll.timestamp) / 1000.0f);
      touchScroll.delta += distance;
      touchScroll.velocity = magister::smoothFlingVelocity(
          touchScroll.velocity,
          std::clamp(distance / seconds, -5000 * uiScale, 5000 * uiScale),
          seconds);
    }
    touchScroll.last = y;
    touchScroll.lastX = x;
    touchScroll.timestamp = timestamp;
  } else if (action == AMOTION_EVENT_ACTION_UP ||
             action == AMOTION_EVENT_ACTION_CANCEL) {
    if (action == AMOTION_EVENT_ACTION_UP && touchScroll.eligible &&
        touchScroll.swiping) {
      requestedDayChange = magister::daySwipe(
          x - touchScroll.startX, y - touchScroll.start, 64 * uiScale);
    }
    touchScroll.down = false;
    if (timestamp - touchScroll.timestamp > 100)
      touchScroll.velocity = 0;
  }
  return 1;
}

void renderFrame() {
  if (!windowReady || !imguiBackends)
    return;
  const std::int64_t frameStart = platform::monotonicMillis();
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplAndroid_NewFrame();
  for (unsigned int codepoint : platform::takeText())
    ImGui::GetIO().AddInputCharacter(codepoint);
  ImGui::NewFrame();
  if (settings.reduceMotion && !touchScroll.down)
    touchScroll.velocity = 0;
  mobileUi();
  ImGui::Render();
  if (requestKeyboard) {
    platform::showKeyboard();
    keyboardVisible = true;
    requestKeyboard = false;
  }
  EGLint width = windowWidth, height = windowHeight;
  eglQuerySurface(eglDisplay, eglSurface, EGL_WIDTH, &width);
  eglQuerySurface(eglDisplay, eglSurface, EGL_HEIGHT, &height);
  if (width > 0)
    windowWidth = width;
  if (height > 0)
    windowHeight = height;
  glViewport(0, 0, width, height);
  glClearColor(.035f, .055f, .10f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  eglSwapBuffers(eglDisplay, eglSurface);
  const bool scrolling = (touchScroll.down && touchScroll.dragging) ||
                         std::abs(touchScroll.velocity) > 10 * uiScale;
  if (scrolling && scrollFrameTimes.size() < 1024) {
    scrollFrameTimes.push_back(
        static_cast<double>(platform::monotonicMillis() - frameStart));
  } else if (!scrolling && !scrollFrameTimes.empty()) {
    std::sort(scrollFrameTimes.begin(), scrollFrameTimes.end());
    double total = 0;
    for (double ms : scrollFrameTimes)
      total += ms;
    platform::log("Scroll frames: %zu, mean %.2f ms, p95 %.2f ms",
                  scrollFrameTimes.size(), total / scrollFrameTimes.size(),
                  scrollFrameTimes[scrollFrameTimes.size() * 95 / 100]);
    scrollFrameTimes.clear();
  }
}

void android_main(android_app *app) {
  gApp = app;
  platform::attach(app);
  app->onAppCmd = handleAppCommand;
  app->onInputEvent = handleInput;
  while (!app->destroyRequested) {
    int events = 0;
    android_poll_source *source = nullptr;
    const int result =
        ALooper_pollOnce(windowReady ? 0 : -1, nullptr, &events,
                         reinterpret_cast<void **>(&source));
    if (result == ALOOPER_POLL_ERROR)
      break;
    if (result >= 0 && source)
      source->process(app, source);
    if (app->destroyRequested)
      break;
    if (windowReady)
      renderFrame();
  }
  if (imguiBackends) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    imguiBackends = false;
  }
  if (imguiInitialized) {
    ImGui::DestroyContext();
    imguiInitialized = false;
  }
  shutdownEgl();
  gApp = nullptr;
}

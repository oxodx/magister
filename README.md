# Magister Native

An Android school client with a C++20 UI and API layer, CMake, native_app_glue,
Dear ImGui and OpenGL ES 3. There is no Python runtime or vendor directory.

## Layout

```
CMakeLists.txt          Native targets and pinned FetchContent dependencies
CMakePresets.json       Direct ARM64 NDK build preset
src/                   App, API, UI and scrolling implementation
app/src/main/          Android manifest, theme and platform adapters
app/build.gradle       APK packaging and CMake invocation
gradle/                Reproducible Gradle wrapper
tests/                 Standalone C++ scroll-physics checks
```

A single MainActivity extends Android's NativeActivity and only bridges the
official WebView login, external browser intents and IME character delivery.
Rendering, input, networking and cookies stay in C++. The Android platform layer
uses native_app_glue, EGL and the ImGui Android backend.

## Build an APK

Install JDK 17, Android SDK platform 33, NDK r28c and CMake 3.22.1. Set
`sdk.dir` in the ignored `local.properties` to your SDK, then run:

```sh
./gradlew assembleDebug
```

Output: `app/build/outputs/apk/debug/app-debug.apk`.
Both ARM64 and ARMv7 are included. Debug builds keep symbols but compile the
native app with optimization so UI performance is representative on a phone.

CMake FetchContent downloads Dear ImGui 1.92.5, nlohmann/json 3.12.0, mbedTLS
3.6.2 and curl 8.11.1 from their upstream release archives, verified with pinned
SHA-256 hashes. HTTPS and persistent cookies are handled by libcurl with mbedTLS
and the Android system certificate store.
The first configure requires internet access. Download archives are cached under
`build/downloads/`; extracted sources and native objects live in generated build
directories. Dependency license files are included in the fetched source archives.

For native-only iteration, set `ANDROID_NDK_HOME`, then run:

```sh
cmake --preset android-arm64
cmake --build --preset android-arm64
```

This builds shared libraries. Gradle is needed only for Android adapters and APK
packaging/signing. Generated outputs, machine-specific settings and caches are
ignored by Git.

## Nightly releases

The `Nightly release` workflow runs daily at 03:00 UTC and can be started
manually. It builds both ABIs, runs the native checks, and publishes the APK as a
prerelease under the rolling `nightly` tag with a date and commit in the filename.
The version code and name are derived from the build date and commit.

To sign nightly APKs with a release key, add these repository secrets:
`ANDROID_KEYSTORE_BASE64` (base64-encoded keystore), `ANDROID_KEYSTORE_PASSWORD`,
`ANDROID_KEY_ALIAS` and `ANDROID_KEY_PASSWORD`. Without them the workflow falls
back to a debug-signed APK.

## Interface and data

The interface, dates, menus and application errors are English. The app requests
English from Magister where supported; school-written content and third-party
login/publisher pages retain the language supplied by those services.

Five fixed destinations keep navigation compact: Schedule, Grades, Tasks,
Attendance and More. More contains messages, books, settings and account actions. The header
menu holds refresh, search and return-to-today. Navigation icons are drawn with
ImGui vector primitives and require no external image or icon package.

- Schedule: swipe between days, use the week strip, see homework markers and
  view all times in the device's local timezone.
- Grades: current-enrollment overview, recent test names and full test details.
- Tasks: assignments within the previous/next 90 days, with detail views.
- Attendance: absence and attendance records are always one tap away.
- More: messages, the textbook catalog, digital publisher links and settings.
- Settings: homework labels, compact schedule cards and reduced motion.

Official Magister web login handles credentials and school two-factor prompts.
Digital books open their authorized publisher link in the browser; the publisher
may require a separate school sign-in.

## Scrolling and checks

Lists use immutable data snapshots and ImGuiListClipper to draw visible rows.
Scroll offsets are applied before BeginChild to avoid the one-frame delay of
SetScrollY. Touch velocity is filtered and momentum uses an analytical exponential
friction model, independent of display refresh rate. Android 11+ receives an NDK
120 Hz frame-rate hint; the OS still decides the actual rate based on the device,
power settings and thermal conditions. Older Android releases retain normal vsync.

Run the standalone checks with a host C++ compiler:

```sh
c++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/scroll_physics_test.cpp -o /tmp/magister-scroll-test
/tmp/magister-scroll-test
```

The checks cover frame-rate-independent travel, immediate drag response, bounds
and velocity filtering. Scroll gestures log frame count, mean and 95th-percentile
frame time to Android logcat without logging school content or authentication data:

```sh
adb logcat -s Magister
```

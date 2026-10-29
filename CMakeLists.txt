cmake_minimum_required(VERSION 3.22.1)
project(magister LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(dependency_downloads "${PROJECT_SOURCE_DIR}/build/downloads")
FetchContent_Declare(sdl
  URL https://codeload.github.com/libsdl-org/SDL/tar.gz/refs/tags/release-2.30.11
  URL_HASH SHA256=cc6136dd964854e8846c679703322f3e2a341d27a06a53f8b3f642c26f1b0cfd
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(imgui
  URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.92.5
  URL_HASH SHA256=0eb50fe9aeba1a51f96b5843c7f630a32ed2e9362d693c61b87e4fa870cf826d
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(nlohmann_json
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
  DOWNLOAD_DIR "${dependency_downloads}")
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sdl imgui nlohmann_json)

add_library(magister SHARED
  src/main.cpp src/magister_client.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(magister PRIVATE src ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_compile_options(magister PRIVATE -Wall -Wextra -Wpedantic
  $<$<CONFIG:Debug>:-O2>)
target_compile_definitions(magister PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(magister PRIVATE SDL2::SDL2 nlohmann_json::nlohmann_json android log dl EGL GLESv3)

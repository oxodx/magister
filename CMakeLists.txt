cmake_minimum_required(VERSION 3.22.1)
project(magister LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(dependency_downloads "${PROJECT_SOURCE_DIR}/build/downloads")
FetchContent_Declare(imgui
  URL https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.92.5
  URL_HASH SHA256=0eb50fe9aeba1a51f96b5843c7f630a32ed2e9362d693c61b87e4fa870cf826d
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(nlohmann_json
  URL https://codeload.github.com/nlohmann/json/tar.gz/refs/tags/v3.12.0
  URL_HASH SHA256=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(mbedtls
  URL https://codeload.github.com/Mbed-TLS/mbedtls/tar.gz/refs/tags/v3.6.2
  URL_HASH SHA256=f4a876b1f6921ad0aefb445f974ef62414d33928640b2c45555c5e64a196a1a8
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(mbedtls_framework
  URL https://codeload.github.com/Mbed-TLS/mbedtls-framework/tar.gz/94599c0e3b5036e086446a51a3f79640f70f22f6
  URL_HASH SHA256=632d5aa4dd2e4179f9d648be12af9a9c3158d7f503a93e70e651751b5f31257c
  DOWNLOAD_DIR "${dependency_downloads}")
FetchContent_Declare(curl
  URL https://codeload.github.com/curl/curl/tar.gz/refs/tags/curl-8_11_1
  URL_HASH SHA256=78baef817c33a0a11b990d031a706dc1486f614b581e22949fc101add32c4cc8
  DOWNLOAD_DIR "${dependency_downloads}")
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_GetProperties(mbedtls_framework)
if(NOT mbedtls_framework_POPULATED)
  FetchContent_Populate(mbedtls_framework)
endif()
FetchContent_GetProperties(mbedtls)
if(NOT mbedtls_POPULATED)
  FetchContent_Populate(mbedtls)
endif()
file(REMOVE_RECURSE "${mbedtls_SOURCE_DIR}/framework")
file(COPY "${mbedtls_framework_SOURCE_DIR}/" DESTINATION "${mbedtls_SOURCE_DIR}/framework")
FetchContent_MakeAvailable(imgui nlohmann_json)
add_subdirectory("${mbedtls_SOURCE_DIR}" "${mbedtls_BINARY_DIR}")
set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ENABLE_EXPORT_TARGET OFF CACHE BOOL "" FORCE)
set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(curl)

add_library(magister SHARED
  src/main.cpp src/magisterClient.cpp src/androidPlatform.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_android.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
set_source_files_properties(
  ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
  PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(magister PRIVATE src ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
  ${CMAKE_ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(magister PRIVATE -Wall -Wextra -Wpedantic
  $<$<CONFIG:Debug>:-O2>)
target_compile_definitions(magister PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(magister PRIVATE CURL::libcurl
  nlohmann_json::nlohmann_json android log dl EGL GLESv3)

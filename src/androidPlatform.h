#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct android_app;

namespace platform {

void attach(android_app *app);
std::string internalDataPath();
void openUrl(const std::string &url);
std::string webLogin(const std::string &url);
std::string loadSchool();
std::string loadSessionToken();
void saveSession(const std::string &school, const std::string &accessToken);
void clearSession();
void showKeyboard();
void hideKeyboard();
void finish();
void addText(unsigned int codepoint);
std::vector<unsigned int> takeText();
void log(const char *format, ...);
std::int64_t monotonicMillis();

} // namespace platform

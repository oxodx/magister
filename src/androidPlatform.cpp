#include "androidPlatform.h"

#include <android/log.h>
#include <android/native_activity.h>
#include <android_native_app_glue.h>
#include <cstdarg>
#include <ctime>
#include <jni.h>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace {

std::mutex appMutex;
android_app *currentApp = nullptr;
std::mutex textMutex;
std::vector<unsigned int> pendingText;

android_app *app() {
  std::lock_guard lock(appMutex);
  if (!currentApp || !currentApp->activity)
    throw std::runtime_error("Android platform is unavailable");
  return currentApp;
}

class ScopedEnv {
public:
  explicit ScopedEnv(android_app *state) : app_(state) {
    if (app_->activity->vm->GetEnv(reinterpret_cast<void **>(&env_),
                                   JNI_VERSION_1_6) == JNI_EDETACHED) {
      if (app_->activity->vm->AttachCurrentThread(&env_, nullptr) != JNI_OK)
        throw std::runtime_error("Unable to attach the native thread to the JVM");
      attached_ = true;
    }
  }

  ~ScopedEnv() {
    if (attached_)
      app_->activity->vm->DetachCurrentThread();
  }

  JNIEnv *get() const { return env_; }
  JNIEnv *operator->() const { return env_; }

private:
  android_app *app_;
  JNIEnv *env_ = nullptr;
  bool attached_ = false;
};

void checkException(JNIEnv *env, const char *message) {
  if (!env->ExceptionCheck())
    return;
  env->ExceptionDescribe();
  env->ExceptionClear();
  throw std::runtime_error(message);
}

std::string fromJString(JNIEnv *env, jstring value) {
  if (!value)
    return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  std::string result(chars ? chars : "");
  if (chars)
    env->ReleaseStringUTFChars(value, chars);
  return result;
}

} // namespace

namespace platform {

void attach(android_app *state) {
  std::lock_guard lock(appMutex);
  currentApp = state;
}

std::string internalDataPath() {
  android_app *state = app();
  return state->activity->internalDataPath ? state->activity->internalDataPath
                                           : "";
}

void openUrl(const std::string &url) {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method =
      env->GetMethodID(activityClass, "openUrl", "(Ljava/lang/String;)V");
  if (!method) {
    checkException(env.get(), "Browser bridge is unavailable");
    throw std::runtime_error("Browser bridge is unavailable");
  }
  jstring address = env->NewStringUTF(url.c_str());
  env->CallVoidMethod(activity, method, address);
  checkException(env.get(), "Unable to open the browser");
  env->DeleteLocalRef(address);
  env->DeleteLocalRef(activityClass);
}

std::string webLogin(const std::string &url) {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(
      activityClass, "webLogin", "(Ljava/lang/String;)Ljava/lang/String;");
  if (!method) {
    checkException(env.get(), "Web login bridge is unavailable");
    throw std::runtime_error("Web login bridge is unavailable");
  }
  jstring input = env->NewStringUTF(url.c_str());
  jobject result = env->CallObjectMethod(activity, method, input);
  checkException(env.get(), "Web login failed");
  std::string callback = fromJString(env.get(), static_cast<jstring>(result));
  env->DeleteLocalRef(input);
  if (result)
    env->DeleteLocalRef(result);
  env->DeleteLocalRef(activityClass);
  if (callback.empty())
    throw std::runtime_error("Web login was cancelled");
  return callback;
}

std::string callStringMethod(const char *name) {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method =
      env->GetMethodID(activityClass, name, "()Ljava/lang/String;");
  if (!method) {
    checkException(env.get(), "Session storage bridge is unavailable");
    throw std::runtime_error("Session storage bridge is unavailable");
  }
  jobject result = env->CallObjectMethod(activity, method);
  checkException(env.get(), "Unable to read the saved session");
  std::string value = fromJString(env.get(), static_cast<jstring>(result));
  if (result)
    env->DeleteLocalRef(result);
  env->DeleteLocalRef(activityClass);
  return value;
}

std::string loadSchool() { return callStringMethod("loadSchool"); }

std::string loadSessionToken() { return callStringMethod("loadSessionToken"); }

void saveSession(const std::string &school, const std::string &accessToken) {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(
      activityClass, "saveSession", "(Ljava/lang/String;Ljava/lang/String;)V");
  if (!method) {
    checkException(env.get(), "Session storage bridge is unavailable");
    throw std::runtime_error("Session storage bridge is unavailable");
  }
  jstring savedSchool = env->NewStringUTF(school.c_str());
  jstring savedToken = env->NewStringUTF(accessToken.c_str());
  env->CallVoidMethod(activity, method, savedSchool, savedToken);
  checkException(env.get(), "Unable to save the session");
  env->DeleteLocalRef(savedToken);
  env->DeleteLocalRef(savedSchool);
  env->DeleteLocalRef(activityClass);
}

void clearSession() {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(activityClass, "clearSession", "()V");
  if (!method) {
    checkException(env.get(), "Session storage bridge is unavailable");
    throw std::runtime_error("Session storage bridge is unavailable");
  }
  env->CallVoidMethod(activity, method);
  checkException(env.get(), "Unable to clear the session");
  env->DeleteLocalRef(activityClass);
}

void showKeyboard(const std::string &text) {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method =
      env->GetMethodID(activityClass, "showKeyboard", "(Ljava/lang/String;)V");
  if (!method) {
    checkException(env.get(), "Keyboard bridge is unavailable");
    throw std::runtime_error("Keyboard bridge is unavailable");
  }
  jstring value = env->NewStringUTF(text.c_str());
  env->CallVoidMethod(activity, method, value);
  checkException(env.get(), "Unable to show the keyboard");
  env->DeleteLocalRef(value);
  env->DeleteLocalRef(activityClass);
}

void hideKeyboard() {
  android_app *state = app();
  ScopedEnv env(state);
  jobject activity = state->activity->clazz;
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = env->GetMethodID(activityClass, "hideKeyboard", "()V");
  if (!method) {
    checkException(env.get(), "Keyboard bridge is unavailable");
    throw std::runtime_error("Keyboard bridge is unavailable");
  }
  env->CallVoidMethod(activity, method);
  checkException(env.get(), "Unable to hide the keyboard");
  env->DeleteLocalRef(activityClass);
}

void finish() {
  android_app *state = app();
  ANativeActivity_finish(state->activity);
}

void addText(unsigned int codepoint) {
  std::lock_guard lock(textMutex);
  pendingText.push_back(codepoint);
}

std::vector<unsigned int> takeText() {
  std::lock_guard lock(textMutex);
  return std::exchange(pendingText, {});
}

void log(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  __android_log_vprint(ANDROID_LOG_INFO, "Magister", format, arguments);
  va_end(arguments);
}

std::int64_t monotonicMillis() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<std::int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

} // namespace platform

extern "C" JNIEXPORT void JNICALL
Java_org_magister_nativeapp_MainActivity_nativeText(JNIEnv *, jobject,
                                                    jint codepoint) {
  if (codepoint >= 0x20 && codepoint != 0x7F)
    platform::addText(static_cast<unsigned int>(codepoint));
}

extern "C" JNIEXPORT void JNICALL
Java_org_magister_nativeapp_MainActivity_nativeBackspace(JNIEnv *, jobject) {
  platform::addText(8);
}

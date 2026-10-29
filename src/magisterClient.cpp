#include "magisterClient.h"

#include <SDL_system.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <iomanip>
#include <jni.h>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace magister {
namespace {

std::string fromJString(JNIEnv *env, jstring value) {
  if (!value)
    return {};
  const char *chars = env->GetStringUTFChars(value, nullptr);
  std::string result(chars ? chars : "");
  if (chars)
    env->ReleaseStringUTFChars(value, chars);
  return result;
}

std::string encode(const std::string &text) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
      out << c;
    else
      out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
  }
  return out.str();
}

std::string randomHex(size_t count) {
  std::random_device device;
  std::mt19937_64 generator(device());
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  while (count >= 16) {
    out << std::setw(16) << generator();
    count -= 16;
  }
  if (count)
    out << std::setw(static_cast<int>(count))
        << (generator() & ((1ULL << (count * 4)) - 1));
  return out.str();
}

std::string decode(const std::string &text) {
  std::string out;
  out.reserve(text.size());
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+')
      out.push_back(' ');
    else if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 &&
             hex(text[i + 2]) >= 0) {
      out.push_back(
          static_cast<char>((hex(text[i + 1]) << 4) | hex(text[i + 2])));
      i += 2;
    } else
      out.push_back(text[i]);
  }
  return out;
}

std::unordered_map<std::string, std::string> queryValues(const std::string &url,
                                                         char marker) {
  std::unordered_map<std::string, std::string> values;
  auto start = url.find(marker);
  if (start == std::string::npos)
    return values;
  std::stringstream stream(url.substr(start + 1));
  for (std::string part; std::getline(stream, part, '&');) {
    auto equal = part.find('=');
    if (equal != std::string::npos)
      values[decode(part.substr(0, equal))] = decode(part.substr(equal + 1));
  }
  return values;
}

std::string stringValue(const Json &object, const char *upper,
                        const char *lower = nullptr) {
  auto read = [&](const char *key) -> std::string {
    if (!key || !object.contains(key) || object[key].is_null())
      return {};
    if (object[key].is_string())
      return object[key].get<std::string>();
    if (object[key].is_number_integer())
      return std::to_string(object[key].get<long long>());
    return {};
  };
  std::string value = read(upper);
  return value.empty() ? read(lower) : value;
}

Json parseOidcConfig(const std::string &script) {
  // Magister has used `oidcConfig = {...}`, `window.oidcConfig = {...}` and
  // minified equivalents. Extract a balanced object rather than depending on
  // whitespace or a trailing semicolon.
  size_t marker = script.find("oidcConfig");
  if (marker == std::string::npos)
    marker = script.find("OIDC_CONFIG");
  size_t begin = script.find('{', marker == std::string::npos ? 0 : marker);
  if (begin == std::string::npos)
    throw std::runtime_error("School configuration contains no JSON object");
  bool quoted = false, escaped = false;
  char quote = 0;
  int depth = 0;
  for (size_t i = begin; i < script.size(); ++i) {
    char c = script[i];
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == quote)
        quoted = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      quoted = true;
      quote = c;
      continue;
    }
    if (c == '{')
      ++depth;
    else if (c == '}' && --depth == 0) {
      std::string object = script.substr(begin, i - begin + 1);
      try {
        return Json::parse(object);
      } catch (...) {
        // Some deployments serve a JavaScript object with unquoted keys
        // or single-quoted strings. Extract the fields we actually use.
        Json result;
        for (const char *key : {"client_id", "redirect_uri", "response_type",
                                "scope", "acr_values"}) {
          std::regex field(std::string("[\\\"']?") + key +
                           "[\\\"']?\\s*:\\s*[\\\"']([^\\\"']+)[\\\"']");
          std::smatch match;
          if (std::regex_search(object, match, field))
            result[key] = match[1].str();
        }
        if (result.contains("client_id") && result.contains("redirect_uri"))
          return result;
        throw std::runtime_error(
            "School configuration format is not supported");
      }
    }
  }
  throw std::runtime_error("School configuration JSON is incomplete");
}

} // namespace

HttpResponse AndroidHttp::request(const std::string &method,
                                  const std::string &url,
                                  const std::string &body,
                                  const Json &headers) const {
  JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (!env || !activity)
    throw std::runtime_error("Android HTTP bridge unavailable");
  jclass clazz = env->GetObjectClass(activity);
  jmethodID call =
      env->GetMethodID(clazz, "httpRequest",
                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/"
                       "String;Ljava/lang/String;)Ljava/lang/String;");
  if (!call)
    throw std::runtime_error("HTTP bridge method unavailable");
  jstring jm = env->NewStringUTF(method.c_str()),
          ju = env->NewStringUTF(url.c_str());
  jstring jb = env->NewStringUTF(body.c_str()),
          jh = env->NewStringUTF(headers.dump().c_str());
  auto result = static_cast<jstring>(
      env->CallObjectMethod(activity, call, jm, ju, jb, jh));
  std::string raw = fromJString(env, result);
  env->DeleteLocalRef(jm);
  env->DeleteLocalRef(ju);
  env->DeleteLocalRef(jb);
  env->DeleteLocalRef(jh);
  if (result)
    env->DeleteLocalRef(result);
  env->DeleteLocalRef(clazz);
  Json parsed = Json::parse(raw.empty() ? "{}" : raw);
  HttpResponse response;
  response.status = parsed.value("status", 0);
  response.url = parsed.value("url", "");
  response.body = parsed.value("body", "");
  response.xsrf = parsed.value("xsrf", "");
  response.error = parsed.value("error", "");
  if (!response.error.empty())
    throw std::runtime_error(response.error);
  return response;
}

std::string AndroidHttp::webLogin(const std::string &url) const {
  JNIEnv *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
  jclass clazz = env->GetObjectClass(activity);
  jmethodID call = env->GetMethodID(clazz, "webLogin",
                                    "(Ljava/lang/String;)Ljava/lang/String;");
  if (!call)
    throw std::runtime_error("Web login bridge unavailable");
  jstring input = env->NewStringUTF(url.c_str());
  auto result =
      static_cast<jstring>(env->CallObjectMethod(activity, call, input));
  std::string callback = fromJString(env, result);
  env->DeleteLocalRef(input);
  if (result)
    env->DeleteLocalRef(result);
  env->DeleteLocalRef(clazz);
  if (callback.empty())
    throw std::runtime_error("Web login was cancelled");
  return callback;
}

Client::Client(AndroidHttp http, std::string school)
    : http_(http), school_(normalizeSchool(std::move(school))) {
  if (school_.empty())
    throw std::runtime_error("Invalid school name");
  schoolServer_ = school_ + ".magister.net";
}

std::string Client::normalizeSchool(std::string value) {
  value.erase(0, value.find_first_not_of(" \t\r\n"));
  value.erase(value.find_last_not_of(" \t\r\n") + 1);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  auto scheme = value.find("://");
  if (scheme != std::string::npos)
    value = value.substr(scheme + 3);
  value = value.substr(0, value.find_first_of("/?"));
  const std::string suffix = ".magister.net";
  if (value.size() > suffix.size() && value.ends_with(suffix))
    value.resize(value.size() - suffix.size());
  if (!std::regex_match(value, std::regex("[a-z0-9][a-z0-9-]*")))
    return {};
  return value;
}

HttpResponse Client::get(const std::string &url, const Json &extra) {
  Json headers = extra;
  headers["Accept"] = "application/json";
  headers["X-Requested-With"] = "XMLHttpRequest";
  if (!accessToken_.empty())
    headers["Authorization"] = "Bearer " + accessToken_;
  auto response = http_.request("GET", url, {}, headers);
  if (!response.xsrf.empty())
    xsrf_ = response.xsrf;
  if (response.status >= 400) {
    std::string detail = response.body;
    detail.erase(std::remove(detail.begin(), detail.end(), '\n'), detail.end());
    if (detail.size() > 160)
      detail.resize(160);
    throw std::runtime_error("GET " + url + " HTTP " +
                             std::to_string(response.status) +
                             (detail.empty() ? "" : ": " + detail));
  }
  return response;
}

void Client::login() {
  const std::string accounts = "https://accounts.magister.net";
  Json openid =
      Json::parse(get(accounts + "/.well-known/openid-configuration").body);
  std::string js = get("https://" + schoolServer_ + "/oidc_config.js").body;
  std::smatch match;
  Json oidc = parseOidcConfig(js);
  std::string clientId = oidc.value("client_id", "");
  std::string redirectUri = oidc.value("redirect_uri", "");
  std::string acrValues = oidc.value("acr_values", "");
  // Current Magister configs build these values with JavaScript string
  // concatenation. The lightweight parser deliberately does not execute JS,
  // so finish the documented hostname-derived expressions here.
  if (clientId == "M6-" ||
      clientId.find("window.location.hostname") != std::string::npos)
    clientId = "M6-" + schoolServer_;
  if (redirectUri == "https://" ||
      redirectUri.find("window.location.hostname") != std::string::npos)
    redirectUri = "https://" + schoolServer_ + "/oidc/redirect_callback.html";
  if (acrValues == "tenant:" ||
      acrValues.find("window.location.hostname") != std::string::npos)
    acrValues = "tenant:" + schoolServer_;
  std::unordered_map<std::string, std::string> params{
      {"client_id", clientId},
      {"redirect_uri", redirectUri},
      {"response_type", oidc.value("response_type", "id_token token")},
      {"scope", oidc.value("scope", "openid profile")},
      {"ui_locales", "en"},
      {"state", randomHex(32)},
      {"nonce", randomHex(32)},
      {"acr_values", acrValues}};
  std::string auth = openid.value("authorization_endpoint", "") + "?";
  bool first = true;
  for (const auto &[k, v] : params) {
    if (!first)
      auth += '&';
    first = false;
    auth += encode(k) + '=' + encode(v);
  }
  auto webCallback = http_.webLogin(auth);
  auto webFragment = queryValues(webCallback, '#');
  if (!webFragment.contains("access_token"))
    throw std::runtime_error("Login completed without access token");
  accessToken_ = webFragment["access_token"];
}

Json Client::api(const std::string &path,
                 const std::unordered_map<std::string, std::string> &params) {
  std::string url = "https://" + schoolServer_ + "/api/" + path;
  if (!params.empty()) {
    url += '?';
    bool first = true;
    for (auto &[k, v] : params) {
      if (!first)
        url += '&';
      first = false;
      url += encode(k) + '=' + encode(v);
    }
  }
  auto response = get(url);
  if (response.status == 401)
    throw std::runtime_error("Session expired");
  return Json::parse(response.body);
}

Json Client::items(const Json &response) const {
  if (response.is_array())
    return response;
  if (response.contains("Items"))
    return response["Items"];
  if (response.contains("items"))
    return response["items"];
  return Json::array();
}

std::string Client::personId() {
  if (person_.is_null() || person_.empty()) {
    // Current Magister web clients resolve the signed-in person through
    // /api/account; /api/personen is no longer a valid collection route.
    Json account = api("account");
    if (account.contains("Persoon"))
      person_ = account["Persoon"];
    else if (account.contains("persoon"))
      person_ = account["persoon"];
    else
      person_ = account;
  }
  std::string id = stringValue(person_, "Id", "id");
  if (id.empty())
    throw std::runtime_error("Person has no id");
  return id;
}
Json Client::appointments(const std::string &from, const std::string &to) {
  return items(api("personen/" + personId() + "/afspraken",
                   {{"van", from}, {"tot", to}}));
}
Json Client::grades() {
  auto enrollments = items(api("leerlingen/" + personId() + "/aanmeldingen/"));
  if (enrollments.empty())
    throw std::runtime_error("No active school enrollment found.");
  const Json *selected = &enrollments[0];
  for (const auto &enrollment : enrollments)
    if (enrollment.value("isHoofdAanmelding", false)) {
      selected = &enrollment;
      break;
    }
  auto enrollmentId = stringValue(*selected, "id", "Id");
  auto rows =
      items(api("personen/" + personId() + "/aanmeldingen/" + enrollmentId +
                    "/cijfers/cijferoverzichtvooraanmelding",
                {{"actievePerioden", "false"},
                 {"alleenBerekendeKolommen", "false"},
                 {"alleenPTAKolommen", "false"},
                 {"peildatum", stringValue(*selected, "einde", "Einde")}}));
  Json result = Json::array();
  for (auto &row : rows) {
    if (row.contains("CijferKolom") &&
        row["CijferKolom"].value("KolomSoort", 1) != 1)
      continue;
    if (stringValue(row, "CijferStr", "Waarde").empty())
      continue;
    row["_EnrollmentId"] = enrollmentId;
    result.push_back(std::move(row));
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const Json &a, const Json &b) {
                     return stringValue(a, "DatumIngevoerd", "ingevoerdOp") >
                            stringValue(b, "DatumIngevoerd", "ingevoerdOp");
                   });
  // Fetch full test names for the most recent screenfuls. Older grades are
  // enriched when opened, keeping the initial request bounded.
  for (size_t i = 0; i < std::min<size_t>(12, result.size()); ++i) {
    try {
      result[i] = details("grades", result[i]);
    } catch (const std::exception
                 &) { /* The overview remains usable; tap retries. */
    }
  }
  return result;
}
Json Client::details(const std::string &kind, const Json &row) {
  Json result = row;
  if (kind == "grades") {
    if (row.contains("Toetsinformatie"))
      return result;
    if (!row.contains("CijferKolom"))
      return result;
    auto id = stringValue(row["CijferKolom"], "Id", "id");
    auto enrollment = stringValue(row, "_EnrollmentId");
    if (id.empty() || enrollment.empty())
      return result;
    auto info = api("personen/" + personId() + "/aanmeldingen/" + enrollment +
                    "/cijfers/extracijferkolominfo/" + id);
    result["Toetsinformatie"] = info;
    return result;
  }
  if (kind == "books")
    return result;
  std::string path;
  if (row.contains("Links") && row["Links"].is_array())
    for (const auto &link : row["Links"]) {
      auto rel = stringValue(link, "Rel", "rel");
      if (rel == "Self" || rel == "self") {
        path = stringValue(link, "Href", "href");
        break;
      }
    }
  const std::string prefix = "https://" + schoolServer_ + "/api/";
  if (path.starts_with(prefix))
    path.erase(0, prefix.size());
  if (path.starts_with("/api/"))
    path.erase(0, 5);
  if (path.starts_with("api/"))
    path.erase(0, 4);
  auto id = stringValue(row, "Id", "id");
  if (path.empty() && !id.empty()) {
    if (kind == "schedule")
      path = "personen/" + personId() + "/afspraken/" + id;
    if (kind == "assignments")
      path = "personen/" + personId() + "/opdrachten/" + id;
    if (kind == "messages")
      path = "berichten/berichten/" + id;
  }
  if (!path.empty() && path.find(':') == std::string::npos &&
      path.find("..") == std::string::npos && !path.starts_with('/')) {
    auto info = api(path);
    if (info.is_object())
      result.update(info);
  }
  return result;
}
Json Client::assignments(const std::string &from, const std::string &to) {
  return items(api("personen/" + personId() + "/opdrachten",
                   {{"startdatum", from}, {"einddatum", to}}));
}
Json Client::absences(const std::string &from, const std::string &to) {
  return items(api("personen/" + personId() + "/absenties",
                   {{"van", from}, {"tot", to}}));
}
Json Client::books() {
  return items(api("personen/" + personId() + "/lesmateriaal"));
}
std::string Client::bookUrl(const Json &book) {
  if (!book.contains("Links") || !book["Links"].is_array())
    throw std::runtime_error(
        "This book has no digital access through Magister.");
  for (const auto &link : book["Links"]) {
    if (stringValue(link, "Rel", "rel") != "content")
      continue;
    std::string path = stringValue(link, "Href", "href");
    const std::string prefix = "https://" + schoolServer_ + "/api/";
    if (path.starts_with(prefix))
      path.erase(0, prefix.size());
    else if (path.starts_with("/api/"))
      path.erase(0, 5);
    else if (path.starts_with("api/"))
      path.erase(0, 4);
    if (path.empty() || path.starts_with('/') ||
        path.find(":") != std::string::npos ||
        path.find("..") != std::string::npos)
      throw std::runtime_error("Invalid textbook access link.");
    auto response = api(path, {{"redirect_type", "body"}});
    std::string url = stringValue(response, "location", "Location");
    if (!url.starts_with("https://"))
      throw std::runtime_error(
          "The publisher did not return a valid digital access link.");
    return url;
  }
  throw std::runtime_error(
      "This is a physical textbook; no digital book is linked.");
}
Json Client::messages() {
  Json folders = items(api("berichten/mappen"));
  if (folders.empty())
    return Json::array();
  std::string id = stringValue(folders[0], "Id", "id");
  for (auto &f : folders) {
    std::string name = stringValue(f, "Naam", "naam");
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (name.find("inbox") != std::string::npos ||
        name.find("postvak in") != std::string::npos) {
      id = stringValue(f, "Id", "id");
      break;
    }
  }
  return items(api("berichten/mappen/" + id + "/berichten", {{"top", "50"}}));
}

} // namespace magister

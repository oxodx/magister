#pragma once

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace magister {
using Json = nlohmann::json;

// Process-wide response cache shared by every client, so re-sign-ins and tab
// switches reuse data. Entries are stored with their age; each lookup states
// the oldest data it will accept.
namespace cache {
void open(const std::string &path); // loads persisted entries
bool lookup(const std::string &key, double maxAgeSeconds, Json &out);
void store(const std::string &key, const Json &value);
void save(); // persists entries, dropping ones past the retention limit
void erase(); // sign-out: clears memory and the file on disk
} // namespace cache

struct HttpResponse {
  int status = 0;
  std::string url;
  std::string body;
  std::string xsrf;
  std::string error;
};

class HttpClient {
public:
  HttpClient() = default;
  HttpResponse request(const std::string &method, const std::string &url,
                       const std::string &body = {},
                       const Json &headers = Json::object()) const;
  std::string webLogin(const std::string &url) const;
};

class Client {
public:
  Client(HttpClient http, std::string school);

  void login();
  void restoreSession(std::string accessToken);
  const std::string &accessToken() const;
  Json appointments(const std::string &from, const std::string &to);
  Json grades();
  Json assignments(const std::string &from, const std::string &to);
  Json books();
  std::string bookUrl(const Json &book);
  Json details(const std::string &kind, const Json &row);
  Json messages();
  Json absences(const std::string &from, const std::string &to);

  // Reads the agenda from the local cache only, without touching the network,
  // so it can be shown while the live refresh is still running.
  bool cachedAppointments(const std::string &from, const std::string &to,
                          Json &out);
  // Manual refresh: skip cached reads. Fresh responses are still stored.
  void setBypassCache(bool bypass) { bypassCache_ = bypass; }

  static std::string normalizeSchool(std::string value);

private:
  HttpResponse get(const std::string &url,
                   const Json &headers = Json::object());
  std::string apiUrl(const std::string &path,
                     const std::unordered_map<std::string, std::string> &params)
      const;
  // ttlSeconds == 0 means the response is never read from or written to the
  // cache (used for short-lived links).
  Json api(const std::string &path,
           const std::unordered_map<std::string, std::string> &params = {},
           double ttlSeconds = 0);
  Json items(const Json &response) const;
  std::string personId();

  HttpClient http_;
  std::string school_;
  std::string schoolServer_;
  std::string accessToken_;
  std::string xsrf_;
  Json person_;
  bool bypassCache_ = false;
};

} // namespace magister

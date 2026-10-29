#pragma once

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

namespace magister {
using Json = nlohmann::json;

struct HttpResponse {
  int status = 0;
  std::string url;
  std::string body;
  std::string xsrf;
  std::string error;
};

class AndroidHttp {
public:
  AndroidHttp() = default;
  HttpResponse request(const std::string &method, const std::string &url,
                       const std::string &body = {},
                       const Json &headers = Json::object()) const;
  std::string webLogin(const std::string &url) const;
};

class Client {
public:
  Client(AndroidHttp http, std::string school);

  void login();
  Json appointments(const std::string &from, const std::string &to);
  Json grades();
  Json assignments(const std::string &from, const std::string &to);
  Json books();
  std::string bookUrl(const Json &book);
  Json details(const std::string &kind, const Json &row);
  Json messages();
  Json absences(const std::string &from, const std::string &to);

  static std::string normalizeSchool(std::string value);

private:
  HttpResponse get(const std::string &url,
                   const Json &headers = Json::object());
  Json api(const std::string &path,
           const std::unordered_map<std::string, std::string> &params = {});
  Json items(const Json &response) const;
  std::string personId();

  AndroidHttp http_;
  std::string school_;
  std::string schoolServer_;
  std::string accessToken_;
  std::string xsrf_;
  Json person_;
};

} // namespace magister

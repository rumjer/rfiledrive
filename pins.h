#pragma once

#include "httplib.h"
#include "mainfunctions.h"
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

inline fs::path pinsFile() {
  std::string s = baseDir().string();
  while (s.size() > 1 && s.back() == '/')
    s.pop_back();
  fs::path preferred = fs::path(s + "-pins.json");
  std::error_code ec;
  if (fs::exists(preferred, ec))
    return preferred;

  {
    std::ofstream test(preferred, std::ios::app);
    if (test.is_open()) {
      test.close();
      return preferred;
    }
  }

  return fs::current_path() / "pins.json";
}

inline std::mutex &pinsMutex() {
  static std::mutex m;
  return m;
}

inline json readPins() {
  std::ifstream in(pinsFile());
  if (!in)
    return json::array();
  std::stringstream ss;
  ss << in.rdbuf();
  json j = json::parse(ss.str(), nullptr, false);
  if (j.is_discarded() || !j.is_array())
    return json::array();
  return j;
}

inline bool writePins(const json &list) {
  fs::path tmp = pinsFile();
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out)
      return false;
    out << list.dump();
    if (!out)
      return false;
  }
  std::error_code ec;
  fs::rename(tmp, pinsFile(), ec);
  return !ec;
}

inline void registerPinRoutes(httplib::Server &svr) {
  svr.Get("/api/pins", [](const httplib::Request &, httplib::Response &res) {
    std::lock_guard<std::mutex> lock(pinsMutex());
    res.set_header("Cache-Control", "no-store");
    res.set_content(readPins().dump(), "application/json");
  });

  svr.Put("/api/pins", [](const httplib::Request &req, httplib::Response &res) {
    json in = json::parse(req.body, nullptr, false);
    if (in.is_discarded() || !in.is_array() || in.size() > 200) {
      res.status = 400;
      res.set_content("invalid request", "text/plain");
      return;
    }
    json clean = json::array();
    std::set<std::string> seen;
    for (const auto &v : in) {
      if (!v.is_string())
        continue;
      std::string p = v.get<std::string>();
      if (p.empty() || safePath(p).empty())
        continue;
      if (seen.insert(p).second)
        clean.push_back(p);
    }
    std::lock_guard<std::mutex> lock(pinsMutex());
    if (!writePins(clean)) {
      res.status = 500;
      res.set_content("failed", "text/plain");
      return;
    }
    res.set_content(clean.dump(), "application/json");
  });
}

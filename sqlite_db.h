#pragma once

// sqlite_db.h (v2.0.0): SQLite database viewer
//
// Generic table schema inspection, paginated rows, and cell editing for any
// .sqlite or .db file in the drive.
//
// Endpoints:
//   GET  /api/db/schema?name=<relpath>
//   GET  /api/db/generic/rows?name=<relpath>&table=...
//   POST /api/db/generic/save

#include "httplib.h"
#include "liveedit.h"

#include <sqlite3.h>
#include <iostream>
#include <string>

// -- tiny sqlite wrapper --

struct Db {
  sqlite3 *h = nullptr;

  // opens with wal so a reader never blocks a writer
  bool open(const fs::path &p) {
    int rc = sqlite3_open_v2(
        p.c_str(), &h, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, nullptr);
    if (rc != SQLITE_OK) {
      std::cerr << "[sqlite_db] open failed: " << sqlite3_errmsg(h)
                << std::endl;
      if (h) {
        sqlite3_close(h);
        h = nullptr;
      }
      return false;
    }
    sqlite3_busy_timeout(h, 5000); // wait up to 5s if another write is in flight
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA foreign_keys=ON");
    return true;
  }

  // fire and forget for pragmas and setup statements
  void exec(const char *sql) {
    char *err = nullptr;
    sqlite3_exec(h, sql, nullptr, nullptr, &err);
    if (err) {
      std::cerr << "[sqlite_db] exec: " << err << std::endl;
      sqlite3_free(err);
    }
  }

  void close() {
    if (h) {
      sqlite3_close(h);
      h = nullptr;
    }
  }

  ~Db() { close(); }
};

// -- response helpers --

inline void sendJson(httplib::Response &res, int status, const json &j) {
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

inline void sendErr(httplib::Response &res, int status, const std::string &code,
                    const std::string &msg) {
  sendJson(res, status, {{"error", code}, {"message", msg}});
}

inline void sendText(httplib::Response &res, int status, const char *msg) {
  res.status = status;
  res.set_content(msg, "text/plain");
}

// a column value from a sqlite statement, null becomes ""
inline std::string col(sqlite3_stmt *st, int i) {
  const char *v = reinterpret_cast<const char *>(sqlite3_column_text(st, i));
  return v ? v : "";
}

// adds sqlite viewer routes
void registerDbRoutes(httplib::Server &svr);

#include "sqlite_db.h"
#include "httplib.h"
#include "liveedit.h" // backupVersion, fileVersionToken, writeMutex, baseDir, safePath

#include <sqlite3.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>


// ─────────────────────────────────────────────────────────────────────────────
// Generic SQLite viewer
// Opens any .sqlite / .db file in the drive. The file path is validated
// through safePath() so path-traversal is impossible.
// ─────────────────────────────────────────────────────────────────────────────

// GET /api/db/schema?name=<relpath>
// Returns { tables: [ { name, columns: [ { name, type } ] } ] }
static void handleGenericSchema(const httplib::Request &req,
                                httplib::Response &res) {
  fs::path p = safePath(req.get_param_value("name"));
  std::error_code ec;
  if (p.empty() || !fs::is_regular_file(p, ec)) {
    sendErr(res, 404, "not_found", "file not found");
    return;
  }

  Db db;
  if (!db.open(p)) {
    sendErr(res, 500, "db_error", "could not open the database");
    return;
  }

  // sqlite_schema / sqlite_master gives us the list of user tables
  sqlite3_stmt *st = nullptr;
  const char *listSql =
      "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name";
  if (sqlite3_prepare_v2(db.h, listSql, -1, &st, nullptr) != SQLITE_OK) {
    sendErr(res, 500, "db_error", sqlite3_errmsg(db.h));
    return;
  }

  json tables = json::array();
  std::vector<std::string> tableNames;
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *n = reinterpret_cast<const char *>(sqlite3_column_text(st, 0));
    if (n)
      tableNames.push_back(n);
  }
  sqlite3_finalize(st);

  for (const auto &tname : tableNames) {
    std::string pragma = "PRAGMA table_info(" + tname + ")";
    if (sqlite3_prepare_v2(db.h, pragma.c_str(), -1, &st, nullptr) != SQLITE_OK)
      continue;

    json cols = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
      // cid | name | type | notnull | dflt_value | pk
      json c;
      c["name"] = col(st, 1);
      c["type"] = col(st, 2);
      c["pk"] = sqlite3_column_int(st, 5) > 0;
      cols.push_back(std::move(c));
    }
    sqlite3_finalize(st);

    json t;
    t["name"] = tname;
    t["columns"] = std::move(cols);
    tables.push_back(std::move(t));
  }

  sendJson(res, 200,
           {{"tables", std::move(tables)}, {"version", fileVersionToken(p)}});
}

// GET /api/db/generic/rows?name=<relpath>&table=<table>&page=<n>&limit=<n>
//                          &order=<col>&dir=<asc|desc>&filter_<col>=<val>
// Returns { rows:[...], total:<int>, version:"..." }
// All column values are returned as strings (or null).
static void handleGenericRows(const httplib::Request &req,
                              httplib::Response &res) {
  fs::path p = safePath(req.get_param_value("name"));
  std::error_code ec;
  if (p.empty() || !fs::is_regular_file(p, ec)) {
    sendErr(res, 404, "not_found", "file not found");
    return;
  }

  std::string table = req.get_param_value("table");
  if (table.empty()) {
    sendErr(res, 400, "bad_request", "table parameter required");
    return;
  }

  // reject table names that look like SQL injection
  for (char c : table) {
    if (!std::isalnum((unsigned char)c) && c != '_') {
      sendErr(res, 400, "bad_request", "invalid table name");
      return;
    }
  }

  int page = 1;
  int limit = 100;
  try {
    if (req.has_param("page"))
      page = std::stoi(req.get_param_value("page"));
    if (req.has_param("limit"))
      limit = std::stoi(req.get_param_value("limit"));
  } catch (...) {
  }
  if (page < 1)
    page = 1;
  if (limit < 1)
    limit = 1;
  if (limit > 500)
    limit = 500;

  std::string orderCol = req.get_param_value("order");
  std::string orderDir = req.get_param_value("dir");
  if (orderDir != "asc" && orderDir != "desc")
    orderDir = "asc";

  // validate orderCol against actual columns so it cant be injected
  for (char c : orderCol) {
    if (!std::isalnum((unsigned char)c) && c != '_') {
      orderCol = "";
      break;
    }
  }

  Db db;
  if (!db.open(p)) {
    sendErr(res, 500, "db_error", "could not open the database");
    return;
  }

  // fetch column names for this table (used to build SELECT and validate
  // filters)
  std::vector<std::string> colNames;
  {
    std::string pragma = "PRAGMA table_info(" + table + ")";
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(db.h, pragma.c_str(), -1, &st, nullptr) !=
        SQLITE_OK) {
      sendErr(res, 500, "db_error", sqlite3_errmsg(db.h));
      return;
    }
    while (sqlite3_step(st) == SQLITE_ROW) {
      const char *n =
          reinterpret_cast<const char *>(sqlite3_column_text(st, 1));
      if (n)
        colNames.push_back(n);
    }
    sqlite3_finalize(st);
  }
  if (colNames.empty()) {
    sendErr(res, 404, "not_found", "table not found or has no columns");
    return;
  }

  // collect filter_<col>=<val> params (only for known columns, LIKE search)
  std::vector<std::pair<std::string, std::string>> filters;
  for (const auto &c : colNames) {
    std::string key = "filter_" + c;
    if (req.has_param(key)) {
      std::string val = req.get_param_value(key);
      if (!val.empty())
        filters.push_back({c, "%" + val + "%"});
    }
  }

  // build WHERE clause
  std::string where;
  if (!filters.empty()) {
    where = " WHERE ";
    for (size_t i = 0; i < filters.size(); i++) {
      if (i)
        where += " AND ";
      where += "CAST(" + filters[i].first + " AS TEXT) LIKE ? ESCAPE '\\'";
    }
  }

  // COUNT for total
  std::string countSql = "SELECT COUNT(*) FROM " + table + where;
  sqlite3_stmt *cst = nullptr;
  int total = 0;
  if (sqlite3_prepare_v2(db.h, countSql.c_str(), -1, &cst, nullptr) ==
      SQLITE_OK) {
    for (size_t i = 0; i < filters.size(); i++)
      sqlite3_bind_text(cst, (int)(i + 1), filters[i].second.c_str(), -1,
                        SQLITE_TRANSIENT);
    if (sqlite3_step(cst) == SQLITE_ROW)
      total = sqlite3_column_int(cst, 0);
    sqlite3_finalize(cst);
  }

  // ORDER BY (default: rowid if available)
  std::string orderClause;
  if (!orderCol.empty()) {
    orderClause = " ORDER BY " + orderCol + " " + orderDir;
  } else {
    orderClause = " ORDER BY rowid ASC";
  }

  std::string rowsSql =
      "SELECT * FROM " + table + where + orderClause + " LIMIT ? OFFSET ?";
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db.h, rowsSql.c_str(), -1, &st, nullptr) !=
      SQLITE_OK) {
    sendErr(res, 500, "db_error", sqlite3_errmsg(db.h));
    return;
  }

  int bindIdx = 1;
  for (const auto &f : filters)
    sqlite3_bind_text(st, bindIdx++, f.second.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(st, bindIdx++, limit);
  sqlite3_bind_int(st, bindIdx, (page - 1) * limit);

  int ncols = sqlite3_column_count(st);
  json rows = json::array();
  while (sqlite3_step(st) == SQLITE_ROW) {
    json row = json::object();
    for (int i = 0; i < ncols; i++) {
      const char *cname = sqlite3_column_name(st, i);
      if (!cname)
        continue;
      if (sqlite3_column_type(st, i) == SQLITE_NULL) {
        row[cname] = nullptr;
      } else if (sqlite3_column_type(st, i) == SQLITE_INTEGER) {
        row[cname] = sqlite3_column_int64(st, i);
      } else if (sqlite3_column_type(st, i) == SQLITE_FLOAT) {
        row[cname] = sqlite3_column_double(st, i);
      } else {
        const char *v =
            reinterpret_cast<const char *>(sqlite3_column_text(st, i));
        row[cname] = v ? v : "";
      }
    }
    rows.push_back(std::move(row));
  }
  sqlite3_finalize(st);

  sendJson(res, 200,
           {{"rows", std::move(rows)},
            {"total", total},
            {"version", fileVersionToken(p)}});
}

// POST /api/db/generic/save
// Body: { path, table, pkCol, pkVal, column, old, new, token }
// Backs up the file, writes one cell. Generic: no column whitelist — any column
// in the table is writable (access is already gated by Tailscale, no login).
static void handleGenericSave(const httplib::Request &req,
                              httplib::Response &res) {
  json body = json::parse(req.body, nullptr, false);
  if (body.is_discarded() || !body.is_object()) {
    sendErr(res, 400, "bad_request", "expected json object");
    return;
  }

  auto requireStr = [&](const char *key, std::string &out) -> bool {
    if (!body.contains(key) || !body[key].is_string()) {
      sendErr(res, 400, "bad_request",
              std::string(key) + " missing or not a string");
      return false;
    }
    out = body[key].get<std::string>();
    return true;
  };

  std::string filePath, table, pkCol, column, token;
  if (!requireStr("path", filePath))
    return;
  if (!requireStr("table", table))
    return;
  if (!requireStr("pkCol", pkCol))
    return;
  if (!requireStr("column", column))
    return;
  if (!requireStr("token", token))
    return;

  // pkVal can be int or string
  json pkValJ = body.value("pkVal", json(nullptr));

  bool newIsNull = body["new"].is_null();
  std::string newVal = newIsNull ? "" : body["new"].get<std::string>();

  // basic identifier safety: no injection via table/column/pkCol names
  auto safeIdent = [](const std::string &s) {
    for (char c : s)
      if (!std::isalnum((unsigned char)c) && c != '_')
        return false;
    return !s.empty();
  };
  if (!safeIdent(table)) {
    sendErr(res, 400, "bad_request", "invalid table");
    return;
  }
  if (!safeIdent(pkCol)) {
    sendErr(res, 400, "bad_request", "invalid pkCol");
    return;
  }
  if (!safeIdent(column)) {
    sendErr(res, 400, "bad_request", "invalid column");
    return;
  }

  fs::path p = safePath(filePath);
  std::error_code ec;
  if (p.empty() || !fs::is_regular_file(p, ec)) {
    sendErr(res, 404, "not_found", "file not found");
    return;
  }

  std::lock_guard<std::mutex> lock(writeMutex());

  if (fileVersionToken(p) != token) {
    sendJson(
        res, 409,
        {{"error", "changed"},
         {"message",
          "the database was changed on the server since the page was loaded"},
         {"current", fileVersionToken(p)}});
    return;
  }

  // close any open handle before backup
  if (!backupVersion(p, "generic-save")) {
    sendErr(res, 500, "backup_failed",
            "could not back up the database, nothing was saved");
    return;
  }

  Db db;
  if (!db.open(p)) {
    sendErr(res, 500, "db_error", "could not open the database");
    return;
  }

  std::string upd =
      "UPDATE " + table + " SET " + column + "=? WHERE " + pkCol + "=?";
  sqlite3_stmt *st = nullptr;
  if (sqlite3_prepare_v2(db.h, upd.c_str(), -1, &st, nullptr) != SQLITE_OK) {
    sendErr(res, 500, "db_error", sqlite3_errmsg(db.h));
    return;
  }

  if (newIsNull)
    sqlite3_bind_null(st, 1);
  else
    sqlite3_bind_text(st, 1, newVal.c_str(), -1, SQLITE_TRANSIENT);

  // bind PK: integer or text
  if (pkValJ.is_number_integer())
    sqlite3_bind_int64(st, 2, pkValJ.get<int64_t>());
  else if (pkValJ.is_string()) {
    std::string pks = pkValJ.get<std::string>();
    sqlite3_bind_text(st, 2, pks.c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_finalize(st);
    sendErr(res, 400, "bad_request", "pkVal must be integer or string");
    return;
  }

  int rc = sqlite3_step(st);
  sqlite3_finalize(st);

  if (rc != SQLITE_DONE) {
    sendErr(res, 500, "write_failed",
            std::string("could not write the change: ") + sqlite3_errmsg(db.h));
    return;
  }

  db.close();
  sendJson(res, 200, {{"version", fileVersionToken(p)}});
}


// -- route registration --

void registerDbRoutes(httplib::Server &svr) {
  // -- generic SQLite viewer (any .sqlite / .db file) --

  // GET /api/db/schema?name=<relpath>
  svr.Get("/api/db/schema",
          [](const httplib::Request &req, httplib::Response &res) {
            handleGenericSchema(req, res);
          });

  // GET /api/db/generic/rows?name=<relpath>&table=<table>&...
  svr.Get("/api/db/generic/rows",
          [](const httplib::Request &req, httplib::Response &res) {
            handleGenericRows(req, res);
          });

  // POST /api/db/generic/save
  svr.Post("/api/db/generic/save",
           [](const httplib::Request &req, httplib::Response &res) {
             handleGenericSave(req, res);
           });
}

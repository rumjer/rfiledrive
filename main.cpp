#include "httplib.h"
#include "liveedit.h"
#include "mainfunctions.h"
#include "sqlite_db.h"

#ifdef HAS_HARDCODEDCODE
#include "hardcodedcode.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <string>

// time libraries and stuff

#include <chrono>
#include <thread>

// helper

static void sendResult(httplib::Response &res, OpResult r, const char *okMsg) {
  switch (r) {
  case OpResult::Ok:
    res.set_content(okMsg, "text/plain");
    break;
  case OpResult::NotFound:
    res.status = 404;
    res.set_content("not found", "text/plain");
    break;
  case OpResult::Exists:
    res.status = 409;
    res.set_content("already exists", "text/plain");
    break;
  case OpResult::Invalid:
    res.status = 400;
    res.set_content("invalid request", "text/plain");
    break;
  case OpResult::Failed:
    res.status = 500;
    res.set_content("failed", "text/plain");
    break;
  }
}

int main() {
  httplib::Server svr;

  // httplib only accepts 100mb bodies by default, which is too small for
  // big uploads and for saving office files from the editor (v2.0)
  svr.set_payload_max_length(MAX_PAYLOAD_BYTES);

  // GET /api/list
  // lists the contents of the directory given by the "path" query param
  // (relative to the base directory); returns the entries as a JSON array
  svr.Get("/api/list", [](const httplib::Request &req, httplib::Response &res) {
    fs::path dir = safePath(req.get_param_value("path"));
    if (dir.empty()) {
      res.status = 403;
      res.set_content("invalid path", "text/plain");
      return;
    }

    json result = toJson(listDir(dir));
    res.set_content(result.dump(), "application/json");
  });

  // POST /api/mkdir
  // creates a new directory name by the name thing
  svr.Post("/api/mkdir",
           [](const httplib::Request &req, httplib::Response &res) {
             fs::path dir = safePath(req.get_param_value("name"));
             if (dir.empty()) {
               res.status = 403;
               res.set_content("invalid path", "text/plain");
               return;
             }

             if (createDir(dir))
               res.set_content("created", "text/plain");
             else
               res.set_content("failed", "text/plain");
           });

  // GET /api/download
  // reads a file named by the name query param and returns its raw bytes
  svr.Get("/api/download",
          [](const httplib::Request &req, httplib::Response &res) {
            fs::path filePath = safePath(req.get_param_value("name"));
            if (filePath.empty()) {
              res.status = 403;
              res.set_content("invalid path", "text/plain");
              return;
            }

            std::string content;
            if (downloadFile(filePath, content))
              res.set_content(content, getMimeType(filePath));
            else {
              res.status = 404;
              res.set_content("not found", "text/plain");
            }
          });

  // POST /api/upload
  // accepts a multipart file upload under the file field and writes it into
  // the subfolder given by the "path" query param (relative to the base
  // directory)
  svr.Post("/api/upload",
           [](const httplib::Request &req, httplib::Response &res) {
             if (!req.form.has_file("file")) {
               res.status = 400;
               res.set_content("no file provided", "text/plain");
               return;
             }

             const auto &file = req.form.get_file("file");
             std::string destDir = req.get_param_value("path");

             // folder uploads send the path inside the folder; normal uploads
             // send nothing, so fall back to just the file name like before
             std::string rel = req.get_param_value("rel");
             if (rel.empty())
               rel = fs::path(file.filename).filename().string();
             if (!validRelPath(rel)) {
               res.status = 400;
               res.set_content("invalid file path", "text/plain");
               return;
             }

             fs::path filePath = safePath((fs::path(destDir) / rel).string());
             if (filePath.empty()) {
               res.status = 403;
               res.set_content("invalid path", "text/plain");
               return;
             }

             if (uploadFile(filePath, file.content))
               res.set_content("uploaded", "text/plain");
             else {
               res.status = 500;
               res.set_content("failed to write", "text/plain");
             }
           });

  // DELETE /api/delete
  // moves a file or directory (name query param) to the trash
  svr.Delete("/api/delete",
             [](const httplib::Request &req, httplib::Response &res) {
               fs::path target = safePath(req.get_param_value("name"));
               if (target.empty() || target == baseDir()) {
                 res.status = 403;
                 res.set_content("invalid path", "text/plain");
                 return;
               }

               if (!fs::exists(target)) {
                 res.status = 404;
                 res.set_content("not found", "text/plain");
                 return;
               }

               if (moveToTrash(target))
                 res.set_content("deleted", "text/plain");
               else {
                 res.status = 500;
                 res.set_content("failed", "text/plain");
               }
             });

  // POST /api/rename?name=<current path>&newName=<new file name>
  svr.Post("/api/rename",
           [](const httplib::Request &req, httplib::Response &res) {
             fs::path src = safePath(req.get_param_value("name"));
             if (src.empty()) {
               res.status = 403;
               res.set_content("invalid path", "text/plain");
               return;
             }
             sendResult(res, renameStuff(src, req.get_param_value("newName")),
                        "renamed");
           });

  // POST /api/move?name=<file or folder>&dest=<destination folder>
  svr.Post("/api/move",
           [](const httplib::Request &req, httplib::Response &res) {
             fs::path src = safePath(req.get_param_value("name"));
             fs::path dest = safePath(req.get_param_value("dest"));
             if (src.empty() || dest.empty()) {
               res.status = 403;
               res.set_content("invalid path", "text/plain");
               return;
             }
             sendResult(res, moveStuff(src, dest), "moved");
           });

  // GET /api/preview?name=<heic file>&size=<max pixels, default 1600>
  // converts the heic to a jpeg so any browser can show it
  svr.Get(
      "/api/preview", [](const httplib::Request &req, httplib::Response &res) {
        fs::path filePath = safePath(req.get_param_value("name"));
        if (filePath.empty()) {
          res.status = 403;
          res.set_content("invalid path", "text/plain");
          return;
        }

        std::error_code ec;
        if (!fs::is_regular_file(filePath, ec)) {
          res.status = 404;
          res.set_content("not found", "text/plain");
          return;
        }
        if (!isHeic(filePath)) {
          res.status = 415;
          res.set_content("preview only supports heic/heif", "text/plain");
          return;
        }

        int size = 1600;
        if (req.has_param("size")) {
          try {
            size = std::stoi(req.get_param_value("size"));
          } catch (...) {
          }
        }
        size = std::clamp(size, 64, 4096);

        // decoding is slow, so let the browser reuse a conversion if the file
        // didnt change
        std::string etag =
            "\"" + std::to_string(fs::file_size(filePath, ec)) + "-" +
            std::to_string(
                fs::last_write_time(filePath, ec).time_since_epoch().count()) +
            "-" + std::to_string(size) + "\"";
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "private, no-cache");
        if (req.get_header_value("If-None-Match") == etag) {
          res.status = 304;
          return;
        }

        std::string jpeg;
        if (!heicToJpeg(filePath, size, jpeg)) {
          res.status = 500;
          res.set_content("could not decode image", "text/plain");
          return;
        }
        res.set_content(jpeg, "image/jpeg");
      });

  // GET /api/trash/list
  svr.Get("/api/trash/list",
          [](const httplib::Request &, httplib::Response &res) {
            res.set_content(trashList().dump(), "application/json");
          });

  // POST /api/trash/restore?id=...
  svr.Post("/api/trash/restore",
           [](const httplib::Request &req, httplib::Response &res) {
             if (restoreFromTrash(req.get_param_value("id")))
               res.set_content("restored", "text/plain");
             else {
               res.status = 404;
               res.set_content("not found", "text/plain");
             }
           });

  // DELETE /api/trash/purge?id=...   (one item, forever)
  svr.Delete("/api/trash/purge",
             [](const httplib::Request &req, httplib::Response &res) {
               if (purgeFromTrash(req.get_param_value("id")))
                 res.set_content("purged", "text/plain");
               else {
                 res.status = 404;
                 res.set_content("not found", "text/plain");
               }
             });

  // DELETE /api/trash/empty   (everything, forever)
  svr.Delete("/api/trash/empty",
             [](const httplib::Request &, httplib::Response &res) {
               if (emptyTrash())
                 res.set_content("emptied", "text/plain");
               else {
                 res.status = 500;
                 res.set_content("failed", "text/plain");
               }
             });

  // GET /api/storage
  // returns disk usage stats: total/used/free, folder sizes, top files
  svr.Get("/api/storage", [](const httplib::Request &, httplib::Response &res) {
    res.set_content(storageInfo().dump(), "application/json");
  });

  // v2.0: /api/fileinfo, /api/save and /api/versions (see liveedit.cpp)
  registerLiveEditRoutes(svr);

  // v2.0: /api/db/schema, /api/db/generic/rows, /api/db/generic/save
  registerDbRoutes(svr);

#ifdef HAS_HARDCODEDCODE
  registerHardcodedRoutes(svr);
#endif

  // background cleaner: every 10 minutes remove trash older than 48h
  // (and old file versions, see liveedit.h for the rules)
  std::thread([] {
    cleanStaleTempFiles(); // leftovers from a crash, checked once at startup
    while (true) {
      purgeExpired();
      purgeOldVersions();
      std::this_thread::sleep_for(std::chrono::minutes(10));
    }
  }).detach();

  // (OLD) listen stuff,etc

  const char *www = std::getenv("FILES_WWW");
  const char *host = std::getenv("FILES_HOST");
  const char *port = std::getenv("FILES_PORT");

  svr.set_mount_point("/", www ? www : "./www");
  svr.listen(host ? host : "0.0.0.0", port ? std::stoi(port) : 8080);
}
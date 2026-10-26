#include "liveedit.h"
#include "httplib.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// -- small helpers --

static std::mutex g_writeMutex;

std::mutex &writeMutex() { return g_writeMutex; }

static long long nowSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static long long nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

static std::string lowerExtension(const fs::path &p) {
  std::string ext = p.extension().string();
  for (char &c : ext)
    c = std::tolower((unsigned char)c);
  return ext;
}

// version ids are digits only ms, so a request cant sneak a
// ../ into a path through the id
static bool validVersionId(const std::string &id) {
  if (id.empty() || id.size() > 19)
    return false;
  for (unsigned char c : id)
    if (!std::isdigit(c))
      return false;
  return true;
}

// random part of temp file names
static std::string randomHex() {
  std::random_device rd;
  std::mt19937_64 gen((static_cast<unsigned long long>(rd()) << 32) ^ rd());
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(gen()));
  return buf;
}

// how many seconds ago a file was last written (stays 0 if it cant be read)
static long long fileAgeSeconds(const fs::path &p) {
  struct stat st;
  if (::stat(p.c_str(), &st) != 0)
    return 0;
  return nowSeconds() - static_cast<long long>(st.st_mtim.tv_sec);
}

// flush a file's data to the disk
static bool fsyncFile(const fs::path &p) {
  int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

// flush a folder's entries (so a rename survives a power cut)
static void fsyncDir(const fs::path &dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return;
  ::fsync(fd);
  ::close(fd);
}

// -- format guard --
// looks at the first bytes of a file to tell what kind of file it is.
// an edit must never change that (an .xls must stay an .xls)

enum class FormatClass { Other, Ole2, Zip, Pdf, Access };

static FormatClass detectFormat(const std::string &head) {
  static const unsigned char ole2[8] = {0xD0, 0xCF, 0x11, 0xE0,
                                        0xA1, 0xB1, 0x1A, 0xE1};
  if (head.size() >= 8 && std::memcmp(head.data(), ole2, 8) == 0)
    return FormatClass::Ole2; // old .xls .doc .ppt
  if (head.size() >= 4 && head.compare(0, 2, "PK") == 0 &&
      static_cast<unsigned char>(head[2]) == 0x03 &&
      static_cast<unsigned char>(head[3]) == 0x04)
    return FormatClass::Zip; // .xlsx .docx .pptx .odt ...
  if (head.size() >= 4 && head.compare(0, 4, "%PDF") == 0)
    return FormatClass::Pdf;
  if (head.size() >= 19 && (head.compare(4, 15, "Standard Jet DB") == 0 ||
                            head.compare(4, 15, "Standard ACE DB") == 0))
    return FormatClass::Access; // .mdb .accdb
  return FormatClass::Other;    // csv, txt, anything else
}

static FormatClass formatOfFile(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  char buf[32];
  in.read(buf, sizeof(buf));
  return detectFormat(std::string(buf, static_cast<size_t>(in.gcount())));
}

// true if both files are the same kind (zip, ole2, pdf, access, other)
bool sameFileFormat(const fs::path &a, const fs::path &b) {
  return formatOfFile(a) == formatOfFile(b);
}

// a temp file name in the same folder as target, e.g. .rfd-tmp-<random>.accdb
fs::path tempPathFor(const fs::path &target, const std::string &suffix) {
  return target.parent_path() /
         (std::string(TEMP_PREFIX) + randomHex() + suffix);
}

// -- atomic writing --

bool atomicReplaceFile(const fs::path &target, const fs::path &readyTemp,
                       bool durable) {
  std::error_code ec;

  if (durable && !fsyncFile(readyTemp))
    return false;

  // the new file should look like the old one (permissions)
  if (fs::exists(target, ec)) {
    fs::file_status st = fs::status(target, ec);
    if (!ec)
      fs::permissions(readyTemp, st.permissions(), ec);
  }

  // rename is atomic: readers see the old file or the new one, never half
  if (::rename(readyTemp.c_str(), target.c_str()) != 0)
    return false;

  if (durable)
    fsyncDir(target.parent_path());
  return true;
}

bool atomicWriteBytes(const fs::path &target, const std::string &bytes,
                      bool durable) {
  fs::path tmp =
      target.parent_path() / (std::string(TEMP_PREFIX) + randomHex());

  // O_EXCL: never reuse a temp file that already exists
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (fd < 0)
    return false;

  bool ok = true;
  const char *p = bytes.data();
  size_t left = bytes.size();
  while (left > 0) {
    ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      ok = false; // disk full, etc
      break;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  if (::close(fd) != 0)
    ok = false;

  if (ok)
    ok = atomicReplaceFile(target, tmp, durable);

  if (!ok) {
    std::error_code ec;
    fs::remove(tmp, ec); // dont leave junk behind
  }
  return ok;
}

// -- version token --

std::string fileVersionToken(const fs::path &file) {
  struct stat st;
  if (::stat(file.c_str(), &st) != 0)
    return "";
  long long ns = static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL +
                 st.st_mtim.tv_nsec;
  return std::to_string(static_cast<long long>(st.st_size)) + "-" +
         std::to_string(ns);
}

// -- versions store --

fs::path versionsDir() { return fs::path(baseDir().string() + "-versions"); }

static fs::path versionFolderFor(const fs::path &file) {
  return versionsDir() / fs::relative(file, baseDir());
}

bool backupVersion(const fs::path &file, const std::string &reason) {
  std::error_code ec;

  if (!fs::is_regular_file(file, ec))
    return false;

  fs::path dir = versionFolderFor(file);
  fs::create_directories(dir, ec);
  if (ec)
    return false;

  // id = time in ms, bumped if two backups land in the same ms
  long long id = nowMillis();
  while (fs::exists(dir / (std::to_string(id) + ".bak"), ec))
    id++;
  std::string idStr = std::to_string(id);
  fs::path bak = dir / (idStr + ".bak");
  fs::path note = dir / (idStr + ".json");

  uintmax_t size = fs::file_size(file, ec);
  if (ec)
    return false;

  fs::copy_file(file, bak, fs::copy_options::none, ec);
  if (ec) {
    fs::remove(bak, ec);
    return false;
  }

  // a backup that isnt the same size is worse than none, so check
  uintmax_t copied = fs::file_size(bak, ec);
  if (ec || copied != size) {
    fs::remove(bak, ec);
    return false;
  }
  fsyncFile(bak);

  json j;
  j["id"] = idStr;
  j["path"] = fs::relative(file, baseDir()).generic_string();
  j["savedAt"] = nowSeconds();
  j["size"] = size;
  j["reason"] = reason;

  std::ofstream out(note);
  out << j.dump();
  out.flush();
  if (!out) {
    out.close();
    fs::remove(bak, ec);
    fs::remove(note, ec);
    return false;
  }
  return true;
}

json listVersions(const fs::path &file) {
  json result = json::array();
  std::error_code ec;

  fs::path dir = versionFolderFor(file);
  if (!fs::is_directory(dir, ec))
    return result;

  std::vector<std::pair<long long, json>> items;
  for (const auto &entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".json")
      continue;

    std::string id = entry.path().stem().string();
    if (!validVersionId(id) || !fs::exists(dir / (id + ".bak"), ec))
      continue;

    json note;
    std::ifstream in(entry.path());
    try {
      in >> note;
    } catch (...) {
      continue;
    }
    if (!note.is_object())
      continue;

    json item;
    item["id"] = id;
    item["savedAt"] = note.value("savedAt", 0LL);
    item["size"] = note.value("size", 0ULL);
    item["reason"] = note.value("reason", "");
    items.push_back({std::stoll(id), item});
  }

  std::sort(items.begin(), items.end(),
            [](const auto &a, const auto &b) { return a.first > b.first; });
  for (auto &it : items)
    result.push_back(it.second);
  return result;
}

void purgeOldVersions(int keepNewest, long long maxAgeSeconds) {
  // same lock as saving, so we never delete while a backup is half written
  std::lock_guard<std::mutex> lock(g_writeMutex);
  std::error_code ec;

  fs::path root = versionsDir();
  if (!fs::is_directory(root, ec))
    return;

  std::vector<fs::path> dirs;
  for (const auto &entry : fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec))
    if (entry.is_directory(ec))
      dirs.push_back(entry.path());

  for (const auto &dir : dirs) {
    std::vector<std::pair<long long, std::string>> ids;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec))
        continue;
      std::string ext = entry.path().extension().string();
      std::string id = entry.path().stem().string();
      if (!validVersionId(id))
        continue;

      if (ext == ".json") {
        ids.push_back({std::stoll(id), id});
      } else if (ext == ".bak" && !fs::exists(dir / (id + ".json"), ec)) {
        // a backup without a note = a crash in the middle of a backup
        if (fileAgeSeconds(entry.path()) > 24 * 3600)
          fs::remove(entry.path(), ec);
      }
    }

    // newest first throughout
    std::sort(ids.begin(), ids.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });

    // -- vibecoded hard cap: unconditionally remove everything beyond
    // VERSIONS_MAX_PER_FILE this fires before the age check so rapid db edits
    // cant fill the disk
    for (size_t i = static_cast<size_t>(std::max(VERSIONS_MAX_PER_FILE, 0));
         i < ids.size(); i++) {
      fs::remove(dir / (ids[i].second + ".bak"), ec);
      fs::remove(dir / (ids[i].second + ".json"), ec);
    }
    if (static_cast<int>(ids.size()) > VERSIONS_MAX_PER_FILE)
      ids.resize(static_cast<size_t>(std::max(VERSIONS_MAX_PER_FILE, 0)));

    // -- age-based purge: remove entries outside keepNewest that are old enough
    for (size_t i = static_cast<size_t>(std::max(keepNewest, 0));
         i < ids.size(); i++) {
      fs::path note = dir / (ids[i].second + ".json");
      json j;
      std::ifstream in(note);
      long long savedAt = 0;
      try {
        in >> j;
        savedAt = j.value("savedAt", 0LL);
      } catch (...) {
        // unreadable note: treat it as very old so it can be cleaned up
      }
      if (nowSeconds() - savedAt >= maxAgeSeconds) {
        fs::remove(dir / (ids[i].second + ".bak"), ec);
        fs::remove(note, ec);
      }
    }
  }

  // remove folders that are empty now (longest paths first = children before
  // their parents, so a parent only disappears after all children are gone)
  std::sort(dirs.begin(), dirs.end(), [](const auto &a, const auto &b) {
    return a.string().size() > b.string().size();
  });
  for (const auto &dir : dirs)
    if (fs::is_empty(dir, ec))
      fs::remove(dir, ec);
}

void cleanStaleTempFiles(long long olderThanSeconds) {
  std::error_code ec;
  fs::path root = baseDir();
  if (!fs::is_directory(root, ec))
    return;

  std::vector<fs::path> stale;
  for (const auto &entry : fs::recursive_directory_iterator(
           root, fs::directory_options::skip_permission_denied, ec)) {
    if (!entry.is_regular_file(ec))
      continue;
    if (entry.path().filename().string().rfind(TEMP_PREFIX, 0) != 0)
      continue;
    if (fileAgeSeconds(entry.path()) >= olderThanSeconds)
      stale.push_back(entry.path());
  }

  for (const auto &p : stale)
    fs::remove(p, ec);
}

// -- saving --

SaveResult saveFileSafely(const fs::path &target, const std::string &bytes,
                          const std::string &reason,
                          const std::string &expectedToken, bool strict) {
  if (bytes.size() > MAX_PAYLOAD_BYTES)
    return SaveResult::TooLarge;

  std::lock_guard<std::mutex> lock(g_writeMutex);
  std::error_code ec;

  if (fs::exists(target, ec)) {
    if (!fs::is_regular_file(target, ec))
      return SaveResult::Invalid;

    // check if someone change the file since the client loaded it
    if (!expectedToken.empty() && fileVersionToken(target) != expectedToken)
      return SaveResult::Conflict;

    if (strict) {
      uintmax_t oldSize = fs::file_size(target, ec);
      if (oldSize > 0) {
        // an empty body over a real file is almost certainly a bug
        if (bytes.empty())
          return SaveResult::Invalid;
        if (formatOfFile(target) != detectFormat(bytes.substr(0, 32)))
          return SaveResult::FormatMismatch;
      }
    }

    // the old content is safe before we touch anything
    if (!backupVersion(target, reason))
      return SaveResult::BackupFailed;

    if (!atomicWriteBytes(target, bytes, true))
      return SaveResult::WriteFailed;
    return SaveResult::Ok;
  }

  // the client expected a file that isnt there
  if (!expectedToken.empty())
    return SaveResult::NotFound;

  // brand new file: nothing to lose, so skip the disk flush.
  // it still goes through a temp file so nobody sees half a file
  fs::create_directories(target.parent_path(), ec);
  if (ec)
    return SaveResult::WriteFailed;
  if (!atomicWriteBytes(target, bytes, false))
    return SaveResult::WriteFailed;
  return SaveResult::Ok;
}

// puts an old version back (current file is backed up first)
static SaveResult restoreVersion(const fs::path &target, const std::string &id,
                                 const std::string &expectedToken) {
  if (!validVersionId(id))
    return SaveResult::Invalid;

  std::lock_guard<std::mutex> lock(g_writeMutex);
  std::error_code ec;

  fs::path bak = versionFolderFor(target) / (id + ".bak");
  if (!fs::is_regular_file(bak, ec))
    return SaveResult::NotFound;

  if (fs::exists(target, ec)) {
    if (!fs::is_regular_file(target, ec))
      return SaveResult::Invalid;
    if (!expectedToken.empty() && fileVersionToken(target) != expectedToken)
      return SaveResult::Conflict;
    if (!backupVersion(target, "pre-restore"))
      return SaveResult::BackupFailed;
  } else {
    fs::create_directories(target.parent_path(), ec);
    if (ec)
      return SaveResult::WriteFailed;
  }

  // copy next to the target first, then swap it in
  fs::path tmp =
      target.parent_path() / (std::string(TEMP_PREFIX) + randomHex());
  fs::copy_file(bak, tmp, fs::copy_options::none, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return SaveResult::WriteFailed;
  }
  if (!atomicReplaceFile(target, tmp, true)) {
    fs::remove(tmp, ec);
    return SaveResult::WriteFailed;
  }
  return SaveResult::Ok;
}

// -- routes --

static void sendText(httplib::Response &res, int status, const char *msg) {
  res.status = status;
  res.set_content(msg, "text/plain");
}

bool isAccessFile(const fs::path &p) {
  std::string ext = lowerExtension(p);
  return ext == ".accdb" || ext == ".mdb";
}

// access leaves a .laccdb (or .ldb) next to the database while someone has it
// open, so writing to the file then could corrupt it
bool lockedByOtherApp(const fs::path &p) {
  if (!isAccessFile(p))
    return false;
  std::error_code ec;
  fs::path a = p, b = p;
  a.replace_extension(".laccdb");
  b.replace_extension(".ldb");
  return fs::exists(a, ec) || fs::exists(b, ec);
}

// turns a SaveResult into an http answer
static void replySave(httplib::Response &res, SaveResult r,
                      const fs::path &file) {
  switch (r) {
  case SaveResult::Ok: {
    json j;
    j["version"] = fileVersionToken(file);
    res.set_content(j.dump(), "application/json");
    break;
  }
  case SaveResult::Conflict: {
    // the client shows "file changed, reload" and can read the new token
    json j;
    j["error"] = "changed";
    j["current"] = fileVersionToken(file);
    res.status = 409;
    res.set_content(j.dump(), "application/json");
    break;
  }
  case SaveResult::NotFound:
    sendText(res, 404, "not found");
    break;
  case SaveResult::Invalid:
    sendText(res, 400, "invalid request");
    break;
  case SaveResult::FormatMismatch:
    sendText(res, 415, "the new content is a different kind of file");
    break;
  case SaveResult::TooLarge:
    sendText(res, 413, "too large");
    break;
  case SaveResult::BackupFailed:
    std::cerr << "[liveedit] backup failed for " << file << std::endl;
    sendText(res, 500, "could not back up the old version, nothing was saved");
    break;
  case SaveResult::WriteFailed:
    std::cerr << "[liveedit] write failed for " << file << std::endl;
    sendText(res, 500,
             "could not write the file, the old version is untouched");
    break;
  }
}

void registerLiveEditRoutes(httplib::Server &svr) {

  // GET /api/fileinfo?name=<file>
  // size, modified time and the version token the client sends back when saving
  svr.Get("/api/fileinfo",
          [](const httplib::Request &req, httplib::Response &res) {
            fs::path p = safePath(req.get_param_value("name"));
            if (p.empty()) {
              sendText(res, 403, "invalid path");
              return;
            }

            std::error_code ec;
            struct stat st;
            if (!fs::is_regular_file(p, ec) || ::stat(p.c_str(), &st) != 0) {
              sendText(res, 404, "not found");
              return;
            }

            std::string type = lowerExtension(p);
            if (!type.empty())
              type.erase(0, 1); // drop the dot

            json j;
            j["name"] = p.filename().string();
            j["size"] = static_cast<long long>(st.st_size);
            j["mtimeMs"] = static_cast<long long>(st.st_mtim.tv_sec) * 1000LL +
                           st.st_mtim.tv_nsec / 1000000;
            j["version"] = fileVersionToken(p);
            j["type"] = type;
            j["lockedByOtherApp"] = lockedByOtherApp(p);
            res.set_content(j.dump(), "application/json");
          });

  // POST /api/save?name=<file>&expected=<version token>
  // the request body is the complete new file. the old one is kept as a version
  svr.Post(
      "/api/save", [](const httplib::Request &req, httplib::Response &res) {
        fs::path p = safePath(req.get_param_value("name"));
        if (p.empty() || p == baseDir()) {
          sendText(res, 403, "invalid path");
          return;
        }

        std::string expected = req.get_param_value("expected");
        if (expected.empty()) {
          sendText(res, 400, "missing expected version");
          return;
        }

        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) {
          sendText(res, 404, "not found");
          return;
        }
        if (lockedByOtherApp(p)) {
          sendText(res, 423, "the file is open in another program");
          return;
        }

        replySave(res, saveFileSafely(p, req.body, "save", expected, true), p);
      });

  // GET /api/versions?name=<file>
  // old versions of a file, newest first
  svr.Get("/api/versions",
          [](const httplib::Request &req, httplib::Response &res) {
            fs::path p = safePath(req.get_param_value("name"));
            if (p.empty() || p == baseDir()) {
              sendText(res, 403, "invalid path");
              return;
            }
            res.set_content(listVersions(p).dump(), "application/json");
          });

  // GET /api/versions/download?name=<file>&id=<version id>
  svr.Get("/api/versions/download",
          [](const httplib::Request &req, httplib::Response &res) {
            fs::path p = safePath(req.get_param_value("name"));
            if (p.empty() || p == baseDir()) {
              sendText(res, 403, "invalid path");
              return;
            }

            std::string id = req.get_param_value("id");
            if (!validVersionId(id)) {
              sendText(res, 400, "invalid request");
              return;
            }

            std::string content;
            if (!downloadFile(versionFolderFor(p) / (id + ".bak"), content)) {
              sendText(res, 404, "not found");
              return;
            }
            res.set_content(content, getMimeType(p));
          });

  // POST /api/versions/restore?name=<file>&id=<version id>[&expected=<token>]
  // puts an old version back. the current file is kept as a version first,
  // so a restore can be undone too
  svr.Post("/api/versions/restore",
           [](const httplib::Request &req, httplib::Response &res) {
             fs::path p = safePath(req.get_param_value("name"));
             if (p.empty() || p == baseDir()) {
               sendText(res, 403, "invalid path");
               return;
             }
             if (lockedByOtherApp(p)) {
               sendText(res, 423, "the file is open in another program");
               return;
             }

             replySave(res,
                       restoreVersion(p, req.get_param_value("id"),
                                      req.get_param_value("expected")),
                       p);
           });
}

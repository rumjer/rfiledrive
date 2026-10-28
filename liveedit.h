#pragma once

// NOTE: this file might include random leftover stuff from when i was trying to
// import live editing for accdb and other stuff, ignore that, i was too lazy to remove

#include <cstddef>
#include <mutex>
#include <string>

#include "mainfunctions.h" // fs, json, safePath, baseDir

// forward declaration so this header doesnt pull all httplib.h
namespace httplib {
class Server;
}

// -- settings (change here) --

// biggest request body the server accepts (uploads and saves).
// httplibss own default is only 100mb
constexpr size_t MAX_PAYLOAD_BYTES = 512ULL * 1024 * 1024;

// a version is only deleted if it is BOTH outside the newest N versions of
// that file and older than the max age
constexpr int VERSIONS_KEEP_NEWEST = 30;
constexpr long long VERSIONS_MAX_AGE_SECONDS = 30LL * 24 * 3600;

// hard cap, versions beyond this number are removed unconditionally regardless
// of age. one might make a lot of tiny changes and save them evreytime, you can
// edit this number to your liking, i think 25 is enough. i dont want too much bloat
constexpr int VERSIONS_MAX_PER_FILE = 25;

// leftover temp files older than this are removed (after a crash)
constexpr long long STALE_TEMP_SECONDS = 3600;

// temp files are named <TEMP_PREFIX><random> and live next to the real file
constexpr const char *TEMP_PREFIX = ".rfd-tmp-";

// --

enum class SaveResult {
  Ok,
  NotFound,       // file (or version) doesnt exist
  Invalid,        // not a regular file, or empty content over a real file
  Conflict,       // the file changed since the client loaded it
  FormatMismatch, // new bytes are a different kind of file than the old ones
  TooLarge,
  BackupFailed, // couldnt save the old version, so nothing was changed
  WriteFailed   // couldnt write the new file, old file is untouched
};

// one lock for everything that writes or backs up files.
// backupVersion / atomicWriteBytes / atomicReplaceFile do NOT lock by
// themselves, the caller must hold this (saveFileSafely does it for you)
std::mutex &writeMutex();

// <root>-versions, sits next to the files folder like the trash does
fs::path versionsDir();

// "<size>-<mtime in ns>", changes whenever the file is written.
// the client sends it back when saving so we can tell if someone else
// changed the file in the meantime. empty string = file doesnt exist
std::string fileVersionToken(const fs::path &file);

// copies the current file into the versions folder (caller holds writeMutex)
bool backupVersion(const fs::path &file, const std::string &reason);

// versions of one file, newest first (shape matches what the web page gets)
json listVersions(const fs::path &file);

// removes old versions (see settings above). takes writeMutex itself
void purgeOldVersions(int keepNewest = VERSIONS_KEEP_NEWEST,
                      long long maxAgeSeconds = VERSIONS_MAX_AGE_SECONDS);

// removes leftover temp files under the files folder
void cleanStaleTempFiles(long long olderThanSeconds = STALE_TEMP_SECONDS);

// renames readyTemp over target (atomic). readyTemp must be in the same
// folder as target. durable = also flush to disk before and after, so a power
// cut cant lose the new content. on failure the temp file is left for the
// caller to delete
bool atomicReplaceFile(const fs::path &target, const fs::path &readyTemp,
                       bool durable = true);

// writes bytes to a temp file next to target, then atomicReplaceFile
bool atomicWriteBytes(const fs::path &target, const std::string &bytes,
                      bool durable = true);

// the one function to save a file safely (takes writeMutex).
//   reason        what ends up in the version note (save, upload-overwrite...)
//   expectedToken version token the client loaded, or "" to skip the check
//   strict        edit-save rules: refuse empty content over a real file and
//                 refuse changing the kind of file (zip/ole2/pdf/access)
// a brand new file is just written, an existing one is backed up first
SaveResult saveFileSafely(const fs::path &target, const std::string &bytes,
                          const std::string &reason,
                          const std::string &expectedToken, bool strict);

// true for .accdb / .mdb (used by /api/save to refuse saving while access has
// the file open)
bool isAccessFile(const fs::path &p);
// access leaves a .laccdb / .ldb next to the database while it is open
bool lockedByOtherApp(const fs::path &p);
// a fresh temp file name next to target (suffix = e.g. the file extension)
fs::path tempPathFor(const fs::path &target, const std::string &suffix = "");
// are both files the same kind (zip, ole2, pdf, access, other)?
bool sameFileFormat(const fs::path &a, const fs::path &b);

// adds the /api/fileinfo, /api/save and /api/versions routes
void registerLiveEditRoutes(httplib::Server &svr);

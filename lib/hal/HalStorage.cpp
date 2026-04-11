#include "HalStorage.h"

#include <Logging.h>
#include <SDCardManager.h>

#include <string>
#include <utility>

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}

bool HalStorage::begin() { return SDCard.begin(); }

bool HalStorage::ready() const { return SDCard.ready(); }

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) { return SDCard.listFiles(path, maxFiles); }

String HalStorage::readFile(const char* path) { return SDCard.readFile(path); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  return SDCard.readFileToStream(path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  return SDCard.readFileToBuffer(path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) { return SDCard.writeFile(path, content); }

bool HalStorage::ensureDirectoryExists(const char* path) { return SDCard.ensureDirectoryExists(path); }

FsFile HalStorage::open(const char* path, const oflag_t oflag) { return SDCard.open(path, oflag); }

bool HalStorage::mkdir(const char* path, const bool pFlag) { return SDCard.mkdir(path, pFlag); }

bool HalStorage::exists(const char* path) { return SDCard.exists(path); }

bool HalStorage::remove(const char* path) { return SDCard.remove(path); }

bool HalStorage::rmdir(const char* path) { return SDCard.rmdir(path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  return SDCard.openFileForRead(moduleName, path, file);
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  return SDCard.openFileForWrite(moduleName, path, file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { return SDCard.removeDir(path); }

bool HalStorage::forceRemoveDir(const char* path) {
  // Phase 1: collect all entries before modifying directory.
  // Deleting files while iterating corrupts the FAT32 directory iterator.
  std::vector<std::pair<std::string, bool>> entries;
  {
    auto dir = open(path);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return false;
    }
    char name[128];
    for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
      f.getName(name, sizeof(name));
      entries.push_back({std::string(path) + "/" + name, f.isDirectory()});
      f.close();
    }
    dir.close();
  }

  // Phase 2: delete collected entries
  bool allDeleted = true;
  for (const auto& [childPath, isDir] : entries) {
    if (isDir) {
      if (!forceRemoveDir(childPath.c_str())) {
        LOG_ERR("HAL", "forceRemoveDir: failed subdir: %s", childPath.c_str());
        allDeleted = false;
      }
    } else {
      // Open writable, truncate to free FAT clusters, then remove the empty file.
      // This avoids freeChain inside remove() which can fail and corrupt the chain.
      FsFile f = open(childPath.c_str(), O_WRONLY);
      if (f) {
        f.truncate(0);
        f.sync();
        if (!f.remove()) {
          LOG_ERR("HAL", "forceRemoveDir: remove after truncate failed: %s", childPath.c_str());
          f.close();
          allDeleted = false;
        }
      } else {
        LOG_ERR("HAL", "forceRemoveDir: cannot open for write: %s", childPath.c_str());
        allDeleted = false;
      }
    }
  }
  if (!allDeleted) {
    LOG_ERR("HAL", "forceRemoveDir: not all entries deleted in %s", path);
    return false;
  }
  if (!rmdir(path)) {
    LOG_ERR("HAL", "forceRemoveDir: rmdir failed on %s", path);
    return false;
  }
  return true;
}
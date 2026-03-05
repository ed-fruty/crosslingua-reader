#include "HalStorage.h"

#include <Logging.h>
#include <SDCardManager.h>

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
  auto dir = open(path);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return false;
  }
  bool allDeleted = true;
  char name[128];
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    std::string childPath = std::string(path) + "/" + name;
    bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      if (!forceRemoveDir(childPath.c_str())) allDeleted = false;
    } else {
      if (!remove(childPath.c_str())) {
        // Try to truncate to repair a broken FAT chain, then retry remove
        FsFile tmp = open(childPath.c_str(), O_RDWR | O_TRUNC);
        if (tmp) {
          tmp.close();
          if (!remove(childPath.c_str())) {
            LOG_ERR("HAL", "forceRemoveDir: failed to remove %s", childPath.c_str());
            allDeleted = false;
          }
        } else {
          LOG_ERR("HAL", "forceRemoveDir: failed to truncate %s", childPath.c_str());
          allDeleted = false;
        }
      }
    }
  }
  dir.close();
  return allDeleted && rmdir(path);
}
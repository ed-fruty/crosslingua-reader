#include "OtaUpdater.h"

// clang-format off
// HttpDownloader.h pulls Arduino/SdFat, whose macros collide with lwip's
// ip4_addr.h unless seen first. Pin this order; clang-format would otherwise sort
// the local header last and break the build.
#include "HttpDownloader.h"
#include <Logging.h>
#include <ReleaseJsonParser.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
// clang-format on

#include <cstring>
#include <string>

namespace {
constexpr char latestReleaseUrl[] = "https://api.github.com/repos/ed-fruty/crosslingua-reader/releases/latest";

struct ProductVersion {
  int crossLinguaMajor = 0;
  int crossLinguaMinor = 0;
  int crossLinguaPatch = 0;
  int crossPointMajor = 0;
  int crossPointMinor = 0;
  int crossPointPatch = 0;
  bool valid = false;
};

ProductVersion parseProductVersion(const char* version) {
  ProductVersion parsed;
  if (!version) return parsed;

  if (*version == 'v' || *version == 'V') version++;
  if (sscanf(version, "%d.%d.%d", &parsed.crossLinguaMajor, &parsed.crossLinguaMinor,
             &parsed.crossLinguaPatch) != 3) {
    return parsed;
  }

  const char* crossPoint = strstr(version, "+crosspoint.");
  if (crossPoint) {
    crossPoint += strlen("+crosspoint.");
    // Older/simple CrossLingua builds may omit the upstream version. Treat
    // that as 0.0.0 so a tagged release with the same CrossLingua version and
    // a real CrossPoint base is still considered newer.
    sscanf(crossPoint, "%d.%d.%d", &parsed.crossPointMajor, &parsed.crossPointMinor, &parsed.crossPointPatch);
  }

  parsed.valid = true;
  return parsed;
}

int compareTriplet(int lhsMajor, int lhsMinor, int lhsPatch, int rhsMajor, int rhsMinor, int rhsPatch) {
  if (lhsMajor != rhsMajor) return lhsMajor < rhsMajor ? -1 : 1;
  if (lhsMinor != rhsMinor) return lhsMinor < rhsMinor ? -1 : 1;
  if (lhsPatch != rhsPatch) return lhsPatch < rhsPatch ? -1 : 1;
  return 0;
}
}  // namespace

OtaUpdater::OtaUpdaterError OtaUpdater::checkForUpdate() {
  LOG_DBG("OTA", "Checking for update (current: %s)", CROSSPOINT_VERSION);

  // Stream the ~32KB release JSON straight into the parser as it arrives.
  // Buffering the whole body in a std::string would add a growing allocation
  // on top of the TLS session's heap during the fetch; with -fno-exceptions an
  // OOM there aborts. fetchUrl handles the verified-https GET, redirects, and
  // User-Agent (see HttpDownloader).
  ReleaseJsonParser releaseParser;
  const bool ok = HttpDownloader::fetchUrl(latestReleaseUrl, [&releaseParser](const uint8_t* data, size_t len) {
    releaseParser.feed(reinterpret_cast<const char*>(data), len);
    return true;
  });
  if (!ok) {
    LOG_ERR("OTA", "Release check fetch failed");
    return HTTP_ERROR;
  }

  LOG_DBG("OTA", "Parser results: tag=%s firmware=%s", releaseParser.foundTag() ? "yes" : "no",
          releaseParser.foundFirmware() ? "yes" : "no");

  if (!releaseParser.foundTag()) {
    LOG_ERR("OTA", "No tag_name in release JSON");
    return JSON_PARSE_ERROR;
  }

  if (!releaseParser.foundFirmware()) {
    LOG_ERR("OTA", "No firmware.bin asset found");
    return NO_UPDATE;
  }

  latestVersion = releaseParser.getTagName();
  otaUrl = releaseParser.getFirmwareUrl();
  otaSize = releaseParser.getFirmwareSize();
  totalSize = otaSize;
  updateAvailable = true;

  LOG_DBG("OTA", "Found update: tag=%s size=%zu", latestVersion.c_str(), otaSize);
  LOG_DBG("OTA", "Firmware URL: %s", otaUrl.c_str());
  return OK;
}

bool OtaUpdater::isUpdateNewer() const {
  if (!updateAvailable || latestVersion.empty() || latestVersion == CROSSPOINT_VERSION) {
    return false;
  }

  const ProductVersion current = parseProductVersion(CROSSPOINT_VERSION);
  const ProductVersion latest = parseProductVersion(latestVersion.c_str());
  if (!current.valid || !latest.valid) {
    LOG_ERR("OTA", "Invalid version format: current=%s latest=%s", CROSSPOINT_VERSION, latestVersion.c_str());
    return false;
  }

  const int crossLinguaOrder =
      compareTriplet(latest.crossLinguaMajor, latest.crossLinguaMinor, latest.crossLinguaPatch,
                     current.crossLinguaMajor, current.crossLinguaMinor, current.crossLinguaPatch);
  if (crossLinguaOrder != 0) return crossLinguaOrder > 0;

  const int crossPointOrder =
      compareTriplet(latest.crossPointMajor, latest.crossPointMinor, latest.crossPointPatch, current.crossPointMajor,
                     current.crossPointMinor, current.crossPointPatch);
  if (crossPointOrder != 0) return crossPointOrder > 0;

  // If we reach here, it means all segments are equal.
  // One final check, if we're on an RC build (contains "-rc"), we should consider the latest version as newer even if
  // the segments are equal, since RC builds are pre-release versions.
  if (strstr(CROSSPOINT_VERSION, "-rc") != nullptr) {
    return true;
  }

  return false;
}

const std::string& OtaUpdater::getLatestVersion() const { return latestVersion; }

OtaUpdater::OtaUpdaterError OtaUpdater::installUpdate(ProgressCallback onProgress, void* ctx) {
  if (!isUpdateNewer()) {
    return UPDATE_OLDER_ERROR;
  }

  // esp_https_ota is hardwired to esp-tls/mbedTLS, whose precompiled build on this
  // package can't negotiate TLS 1.3 (see SecureClient.h). Drive the OTA partition
  // ourselves and stream the firmware through HttpDownloader, which runs over
  // wolfSSL when FREEINK_NET_WOLFSSL is set, reusing its redirect handling for the
  // GitHub -> CDN hop.
  const esp_partition_t* updatePartition = esp_ota_get_next_update_partition(nullptr);
  if (!updatePartition) {
    LOG_ERR("OTA", "No OTA partition available");
    return INTERNAL_UPDATE_ERROR;
  }

  esp_ota_handle_t otaHandle = 0;
  esp_err_t esp_err = esp_ota_begin(updatePartition, OTA_SIZE_UNKNOWN, &otaHandle);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_begin failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  /* For better timing and connectivity, we disable power saving for WiFi */
  esp_wifi_set_ps(WIFI_PS_NONE);

  processedSize = 0;
  int lastReportedPct = -1;
  bool flashOk = true;
  const bool fetchOk = HttpDownloader::fetchUrl(otaUrl, [&](const uint8_t* data, size_t len) {
    if (esp_ota_write(otaHandle, data, len) != ESP_OK) {
      flashOk = false;
      return false;  // abort the transfer
    }
    processedSize += len;
    // Fire the callback only on whole-percent change. Per-chunk updates wake the
    // render task, whose framebuffer work contends with TLS on the internal arena,
    // and e-ink can't repaint faster than a percent tick anyway.
    if (onProgress && totalSize > 0) {
      const int pct = static_cast<int>(static_cast<uint64_t>(processedSize) * 100 / totalSize);
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        onProgress(ctx);
      }
    }
    return true;
  });

  /* Return back to default power saving for WiFi in case of failing */
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

  if (!fetchOk || !flashOk) {
    LOG_ERR("OTA", "Firmware install failed (%s)", flashOk ? "download" : "flash write");
    esp_ota_abort(otaHandle);
    return flashOk ? HTTP_ERROR : INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_end(otaHandle);  // verifies the written image
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_end failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  esp_err = esp_ota_set_boot_partition(updatePartition);
  if (esp_err != ESP_OK) {
    LOG_ERR("OTA", "esp_ota_set_boot_partition failed: %s", esp_err_to_name(esp_err));
    return INTERNAL_UPDATE_ERROR;
  }

  LOG_INF("OTA", "Update completed");
  return OK;
}

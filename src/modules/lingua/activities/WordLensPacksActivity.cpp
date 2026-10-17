#include "modules/lingua/activities/WordLensPacksActivity.h"

#include <ArduinoJson.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"
#include "modules/lingua/wordlens/LensBuilder.h"
#include "modules/lingua/wordlens/LensReader.h"
#include "modules/lingua/wordlens/WordLensService.h"
#include "modules/lingua/wordlens/WordLensStorage.h"
#include "network/HttpDownloader.h"

namespace fui = freeink::ui;
using namespace lingua::wordlens;

namespace {

constexpr char MANIFEST_TMP[] = "/.crosspoint/wordlens/manifest.tmp";
constexpr char JSON_TMP[] = "/.crosspoint/wordlens/readest-pack.tmp";

// Source languages the Word Lens tokenizer handles: every space-delimited script. The ones that
// need a word segmenter are refused, as Readest refuses them.
bool supportedSource(const char* code) {
  static constexpr const char* NEED_SEGMENTER[] = {"zh", "ja", "ko", "th"};
  if (code[0] == '\0') return false;
  for (const char* s : NEED_SEGMENTER) {
    if (std::strcmp(s, code) == 0) return false;
  }
  return true;
}

// A manifest value used in a path or URL: letters, digits, '-', '_' and '.', no "..".
bool plainName(const char* s) {
  if (s[0] == '\0' || std::strstr(s, "..") != nullptr) return false;
  for (; *s; s++) {
    const char c = *s;
    const bool ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    if (!ok) return false;
  }
  return true;
}

const char* languageName(const char* code) { return WordLensService::languageName(code); }

void copyField(char* out, const size_t outSize, const char* value) {
  std::snprintf(out, outSize, "%s", value ? value : "");
}

std::string formatSize(const size_t bytes) {
  char buf[24];
  if (bytes >= 1024 * 1024) {
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    std::snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>((bytes + 1023) / 1024));
  }
  return buf;
}

// A pack download or conversion was asked to stop (Back, held or tapped).
bool backPressed(MappedInputManager& input) {
  return input.isPressed(MappedInputManager::Button::Back) || input.wasPressed(MappedInputManager::Button::Back);
}

bool tlsHeapAvailable() {
  return ESP.getFreeHeap() >= HttpDownloader::MIN_TLS_FREE_HEAP &&
         ESP.getMaxAllocHeap() >= HttpDownloader::MIN_TLS_MAX_ALLOC;
}

}  // namespace

WordLensPacksActivity::WordLensPacksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string epubPath, std::string bookLanguage)
    : UiListActivity("WordLensPacks", renderer, mappedInput), epubPath_(std::move(epubPath)) {
  WordLensService::sourceLanguage(bookLanguage, bookSource_, sizeof(bookSource_));
}

WordLensPacksActivity::~WordLensPacksActivity() = default;

const char* WordLensPacksActivity::headerTitle() const { return tr(STR_WORD_LENS_PACKS); }

void WordLensPacksActivity::onEnter() {
  UiListActivity::onEnter();
  startWifi();
}

void WordLensPacksActivity::startWifi() {
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void WordLensPacksActivity::onExit() {
  Activity::onExit();
  if (converter_) {
    converter_->abort();
    converter_.reset();
    Storage.remove(JSON_TMP);
    Storage.remove(stagedPath_.c_str());
  }
  // The font downloader's teardown, step for step: drop the link, then come back to the book on a
  // clean heap through a reboot rather than switching the radio off and reopening the reader.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestartToReader();
  } else if (wifiUsed_) {
    silentRestartToReader();
  }
}

void WordLensPacksActivity::returnToReader() {
  if (wifiUsed_ || WiFi.getMode() != WIFI_MODE_NULL) {
    finish();  // onExit reboots into the reader
    return;
  }
  activityManager.goToReader(epubPath_);
}

// The translation flows' teardown: no request is in flight and the pack is on SD, so the Wi-Fi
// driver and its buffers can go before the converter needs the heap.
void WordLensPacksActivity::stopWifi() {
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
  WiFi.disconnect(false);
  delay(100);
  WiFi.mode(WIFI_OFF);
  delay(100);
  wifiActive_ = false;
  LOG_DBG("WLENS", "Wi-Fi off for conversion, free heap %u", static_cast<unsigned>(ESP.getFreeHeap()));
}

void WordLensPacksActivity::onBackButton() { returnToReader(); }

void WordLensPacksActivity::setError(const StrId message) {
  RenderLock lock(*this);
  errorMessage_ = message;
  state_ = State::Error;
}

void WordLensPacksActivity::onWifiSelectionComplete(const bool success) {
  const int pending = pendingPack_;
  pendingPack_ = -1;
  if (!success) {
    if (packs_.empty()) {
      returnToReader();
      return;
    }
    RenderLock lock(*this);
    state_ = State::List;
    rowsDirty_ = true;
    return;
  }
  wifiUsed_ = true;
  wifiActive_ = true;
  if (!packs_.empty()) {
    // Back from a conversion that switched Wi-Fi off: carry on with the pack that was picked.
    if (pending >= 0 && pending < static_cast<int>(packs_.size())) {
      download(packs_[pending]);
      requestUpdate();
    } else {
      RenderLock lock(*this);
      state_ = State::List;
    }
    return;
  }
  {
    RenderLock lock(*this);
    state_ = State::Loading;
  }
  requestUpdateAndWait();
  if (!loadManifest()) {
    setError(StrId::STR_WORD_LENS_PACK_FAILED);
    requestUpdate();
    return;
  }
  {
    RenderLock lock(*this);
    state_ = State::List;
    rowsDirty_ = true;
    nav.reset();
  }
  requestUpdate();
}

bool WordLensPacksActivity::parseManifest(const char* path, const bool readest) {
  HalFile file;
  if (!Storage.openFileForRead("WLENS", path, file)) return false;
  JsonDocument filter;
  filter["packs"][0]["source"] = true;
  filter["packs"][0]["target"] = true;
  filter["packs"][0]["file"] = true;
  filter["packs"][0]["bytes"] = true;
  filter["packs"][0]["sha256"] = true;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, file, DeserializationOption::Filter(filter));
  file.close();
  if (err) {
    LOG_ERR("WLENS", "Manifest parse error: %s", err.c_str());
    return false;
  }
  JsonArray list = doc["packs"].as<JsonArray>();
  packs_.clear();
  packs_.reserve(list.size());
  for (JsonObject p : list) {
    Pack pack;
    copyField(pack.source, sizeof(pack.source), p["source"] | "");
    copyField(pack.target, sizeof(pack.target), p["target"] | "");
    copyField(pack.file, sizeof(pack.file), p["file"] | "");
    copyField(pack.sha256, sizeof(pack.sha256), p["sha256"] | "");
    pack.bytes = p["bytes"] | 0u;
    pack.fromReadest = readest;
    if (pack.source[0] == '\0' || pack.target[0] == '\0' || pack.file[0] == '\0' || std::strlen(pack.sha256) != 64) {
      continue;
    }
    if (!supportedSource(pack.source) || !plainName(pack.file) || !plainName(pack.target)) continue;
    packs_.push_back(pack);
  }
  // This book's language first, then by pair.
  const char* mine = bookSource_;
  std::sort(packs_.begin(), packs_.end(), [mine](const Pack& a, const Pack& b) {
    const bool am = std::strcmp(a.source, mine) == 0;
    const bool bm = std::strcmp(b.source, mine) == 0;
    if (am != bm) return am;
    const int c = std::strcmp(a.source, b.source);
    return c != 0 ? c < 0 : std::strcmp(a.target, b.target) < 0;
  });
  refreshInstalled();
  return !packs_.empty();
}

bool WordLensPacksActivity::loadManifest() {
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  if (!tlsHeapAvailable()) {
    LOG_ERR("WLENS", "Low heap for manifest (%u free)", static_cast<unsigned>(ESP.getFreeHeap()));
    return false;
  }
  Storage.mkdir(PACK_DIR);
  // The pack release first. No redirect downgrade: this response carries the sha256 values that are
  // the integrity anchor for the (plain-HTTP-redirected) pack downloads.
  if (HttpDownloader::downloadToFile(std::string(PACK_RELEASE_BASE) + PACK_RELEASE_MANIFEST, MANIFEST_TMP) ==
          HttpDownloader::OK &&
      parseManifest(MANIFEST_TMP, false)) {
    Storage.replaceFile(MANIFEST_TMP, MANIFEST_PATH);
    return true;
  }
  LOG_ERR("WLENS", "Release manifest unavailable; trying the Readest CDN");
  if (HttpDownloader::downloadToFile(std::string(READEST_CDN_BASE) + READEST_CDN_MANIFEST, MANIFEST_TMP) ==
          HttpDownloader::OK &&
      parseManifest(MANIFEST_TMP, true)) {
    Storage.replaceFile(MANIFEST_TMP, READEST_MANIFEST_PATH);
    return true;
  }
  Storage.remove(MANIFEST_TMP);
  return false;
}

void WordLensPacksActivity::refreshInstalled() {
  for (Pack& p : packs_) p.installed = WordLensService::isInstalled(p.source, p.target);
}

void WordLensPacksActivity::rebuildRows() {
  rowLabels_.assign(packs_.size() * 2, std::string());
  rowItems_.clear();
  rowItems_.reserve(packs_.size());
  for (size_t i = 0; i < packs_.size(); i++) {
    const Pack& p = packs_[i];
    // The pair gets the row's full width; size, state and source go on the smaller line under it,
    // so a long language name is never cut for a value column in any orientation.
    rowLabels_[2 * i] = std::string(languageName(p.source)) + " \xE2\x86\x92 " + languageName(p.target);
    rowLabels_[2 * i + 1] = p.installed ? std::string(tr(STR_INSTALLED)) : formatSize(p.bytes);
    if (!p.installed && p.fromReadest)
      rowLabels_[2 * i + 1] += std::string(" \xC2\xB7 ") + tr(STR_WORD_LENS_FROM_READEST);
    fui::ListItem item;
    item.label = rowLabels_[2 * i].c_str();
    item.subtitle = rowLabels_[2 * i + 1].c_str();
    item.actionValue = static_cast<int16_t>(i);
    if (p.installed) item.state = fui::StateDisabled;  // dimmed, still tappable (delete)
    rowItems_.push_back(item);
  }
}

void WordLensPacksActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  if (packs_.empty()) {
    screen.centeredText(tr(STR_WORD_LENS_NO_PACKS), screen.theme().bodyText);
    return;
  }
  if (rowsDirty_) {
    rebuildRows();
    rowsDirty_ = false;
  }
  fui::ListProps props;
  props.items = rowItems_.data();
  props.count = static_cast<uint16_t>(rowItems_.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void WordLensPacksActivity::activateIndex(const int index) {
  if (state_ != State::List || index < 0 || index >= static_cast<int>(packs_.size())) return;
  nav.selected = index;
  app.clearTapFlash();
  Pack& pack = packs_[index];
  if (pack.installed) {
    const std::string body = std::string(languageName(pack.source)) + " \xE2\x86\x92 " + languageName(pack.target);
    startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), body),
                           [this, index](const ActivityResult& result) {
                             if (!result.isCancelled && index < static_cast<int>(packs_.size())) {
                               deletePack(packs_[index]);
                             }
                             requestUpdate();
                           });
    return;
  }
  if (!wifiActive_) {
    pendingPack_ = index;
    startWifi();
    return;
  }
  download(pack);
  requestUpdateAndWait();
}

void WordLensPacksActivity::deletePack(const Pack& pack) {
  char path[64];
  packPath(pack.source, pack.target, path, sizeof(path));
  Storage.remove(path);
  refreshInstalled();
  rowsDirty_ = true;
}

bool WordLensPacksActivity::sha256Matches(const char* path, const char* expectedHex) {
  HalFile file;
  if (!Storage.openFileForRead("WLENS", path, file)) return false;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(512);
  if (!buffer) return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, /*is224=*/0);
  for (;;) {
    const int n = file.read(buffer.get(), 512);
    if (n <= 0) break;
    mbedtls_sha256_update(&ctx, buffer.get(), static_cast<size_t>(n));
  }
  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  char hex[65];
  for (int i = 0; i < 32; i++) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return std::strcmp(hex, expectedHex) == 0;
}

void WordLensPacksActivity::pollCancel() {
  mappedInput.update(true);
  if (backPressed(mappedInput)) {
    cancelRequested_ = true;
    swallowBackRelease_ = true;
  }
}

bool WordLensPacksActivity::downloadLens(const Pack& pack, const char* destination) {
  const std::string url = std::string(PACK_RELEASE_BASE) + pack.file;
  const auto result = HttpDownloader::downloadToFile(
      url, destination,
      [this](const size_t done, const size_t total) {
        progressDone_ = done;
        progressTotal_ = total;
        pollCancel();
        requestUpdate(true);
      },
      // The release asset redirect is followed over plain HTTP: the sha256 below (from the
      // TLS-fetched manifest) covers integrity, and a second TLS session would crowd the C3 heap.
      &cancelRequested_, "", "", {}, /*downgradeRedirectsToHttp=*/true);
  if (result != HttpDownloader::OK) return false;
  if (!sha256Matches(destination, pack.sha256)) {
    LOG_ERR("WLENS", "sha256 mismatch: %s", pack.file);
    Storage.remove(destination);
    return false;
  }
  return true;
}

bool WordLensPacksActivity::downloadJson(const Pack& pack) {
  const std::string url = std::string(READEST_CDN_BASE) + pack.file + "?v=" + std::string(pack.sha256, 8);
  const auto result = HttpDownloader::downloadToFile(
      url, JSON_TMP,
      [this](const size_t done, const size_t total) {
        progressDone_ = done;
        progressTotal_ = total;
        pollCancel();
        requestUpdate(true);
      },
      &cancelRequested_);
  if (result != HttpDownloader::OK) return false;
  if (!sha256Matches(JSON_TMP, pack.sha256)) {
    LOG_ERR("WLENS", "sha256 mismatch: %s", pack.file);
    Storage.remove(JSON_TMP);
    return false;
  }
  return true;
}

bool WordLensPacksActivity::startConversion() {
  stopWifi();
  uint32_t jsonBytes = 0;
  {
    HalFile probe;
    if (Storage.openFileForRead("WLENS", JSON_TMP, probe)) jsonBytes = static_cast<uint32_t>(probe.fileSize());
  }
  lensFs_ = makeUniqueNoThrow<HalLensFs>();
  converter_ = makeUniqueNoThrow<LensConverter>();
  if (!lensFs_ || !converter_) {
    LOG_ERR("WLENS", "OOM: pack converter");
    converter_.reset();
    lensFs_.reset();
    return false;
  }
  LensBuildOptions options;
  options.workDir = PACK_DIR;
  options.partitionLog2 = recommendedPartitionLog2(jsonBytes);
  // 8 KB of partition write buffers in all, however many partitions the pack needs.
  options.writeBufferBytes = std::max<size_t>(64, size_t{8192} >> options.partitionLog2);
  const LensBuildStatus status = converter_->begin(*lensFs_, JSON_TMP, stagedPath_.c_str(), options);
  if (status != LensBuildStatus::Ok) {
    LOG_ERR("WLENS", "Conversion did not start: %s", lensBuildStatusName(status));
    converter_.reset();
    lensFs_.reset();
    return false;
  }
  RenderLock lock(*this);
  state_ = State::Converting;
  progressDone_ = 0;
  progressTotal_ = 100;
  return true;
}

// Called from loop(): converter steps for up to ~100 ms, then back to the main loop so input,
// rendering and the idle task keep running.
void WordLensPacksActivity::stepConversion() {
  if (backPressed(mappedInput)) {
    swallowBackRelease_ = true;
    converter_->abort();  // removes its temporary files and output
    converter_.reset();
    lensFs_.reset();
    Storage.remove(JSON_TMP);
    Storage.remove(stagedPath_.c_str());
    RenderLock lock(*this);
    state_ = State::List;
    rowsDirty_ = true;
    return;
  }
  const unsigned long start = millis();
  LensBuildStatus status = LensBuildStatus::Ok;
  while (status == LensBuildStatus::Ok && !converter_->done() && millis() - start < 100) {
    status = converter_->step();
  }
  if (status != LensBuildStatus::Ok || converter_->done()) {
    const bool ok = status == LensBuildStatus::Ok;
    if (ok) {
      const LensBuildStats& stats = converter_->stats();
      LOG_INF("WLENS", "Converted: %u records, min free heap %u", static_cast<unsigned>(stats.records),
              static_cast<unsigned>(ESP.getMinFreeHeap()));
    }
    endConversion(ok);
    return;
  }
  const uint8_t percent = converter_->percent();
  // A partial refresh per 2 %: enough motion without keeping the panel busy.
  if (percent >= progressDone_ + 2) {
    progressDone_ = percent;
    requestUpdate();
  }
}

void WordLensPacksActivity::endConversion(const bool ok) {
  converter_.reset();
  lensFs_.reset();
  Storage.remove(JSON_TMP);
  bool valid = ok;
  if (valid) {
    // The result must at least open as a pack before it replaces anything.
    HalLensSource source;
    LensReader reader;
    valid = source.open(stagedPath_.c_str()) && reader.open(&source);
    reader.close();
    source.close();
  }
  finishDownload(valid);
  requestUpdate();
}

void WordLensPacksActivity::download(Pack& pack) {
  {
    RenderLock lock(*this);
    state_ = State::Downloading;
    activePack_ = static_cast<int>(&pack - packs_.data());
    progressDone_ = 0;
    progressTotal_ = pack.bytes;
    cancelRequested_ = false;
  }
  requestUpdateAndWait();
  if (auto* fcm = renderer.getFontCacheManager()) fcm->releaseSdFontCaches();
  if (!tlsHeapAvailable()) {
    setError(StrId::STR_MEMORY_ERROR);
    return;
  }
  char path[64];
  packPath(pack.source, pack.target, path, sizeof(path));
  stagedPath_ = std::string(path) + ".new";
  if (pack.fromReadest) {
    const bool fetched = downloadJson(pack);
    if (fetched && !cancelRequested_ && startConversion()) return;  // loop() takes it from here
    Storage.remove(JSON_TMP);
    if (!cancelRequested_) {
      finishDownload(false);
      return;
    }
  } else {
    const bool ok = downloadLens(pack, stagedPath_.c_str());
    if (!cancelRequested_) {
      finishDownload(ok);
      return;
    }
  }
  Storage.remove(stagedPath_.c_str());
  RenderLock lock(*this);
  state_ = State::List;
  rowsDirty_ = true;
}

void WordLensPacksActivity::finishDownload(const bool ok) {
  if (activePack_ < 0 || activePack_ >= static_cast<int>(packs_.size())) return;
  const Pack& pack = packs_[activePack_];
  char path[64];
  packPath(pack.source, pack.target, path, sizeof(path));
  if (!ok || !Storage.replaceFile(stagedPath_.c_str(), path)) {
    Storage.remove(stagedPath_.c_str());
    setError(StrId::STR_WORD_LENS_PACK_FAILED);
    return;
  }
  refreshInstalled();
  RenderLock lock(*this);
  rowsDirty_ = true;
  state_ = State::Complete;
}

bool WordLensPacksActivity::handleCustomInput() {
  // The Back press that cancelled a download or conversion must not also leave the list.
  if (swallowBackRelease_) {
    if (mappedInput.isPressed(MappedInputManager::Button::Back)) return true;
    swallowBackRelease_ = false;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) return true;
  }
  switch (state_) {
    case State::List:
      return false;
    case State::Complete:
    case State::Error:
      if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
          mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        {
          RenderLock lock(*this);
          state_ = packs_.empty() ? State::Error : State::List;
          rowsDirty_ = true;
        }
        if (packs_.empty()) {
          returnToReader();
          return true;
        }
        requestUpdate();
      }
      return true;
    case State::Converting:
      if (converter_) stepConversion();
      return true;
    case State::WifiSelection:
    case State::Loading:
    case State::Downloading:
      return true;
  }
  return true;
}

void WordLensPacksActivity::render(RenderLock&& lock) {
  if (state_ == State::List) {
    UiListActivity::render(std::move(lock));
    return;
  }
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_WORD_LENS_PACKS));
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int centerY = (pageHeight - lineHeight) / 2;
  if (state_ == State::Loading || state_ == State::WifiSelection) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_WORD_LENS_LOADING_PACKS));
  } else if (state_ == State::Downloading || state_ == State::Converting) {
    std::string status =
        I18N.get(state_ == State::Converting ? StrId::STR_WORD_LENS_CONVERTING : StrId::STR_DOWNLOADING);
    if (activePack_ >= 0 && activePack_ < static_cast<int>(packs_.size())) {
      status += std::string(" ") + languageName(packs_[activePack_].target);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, status.c_str());
    const int percent =
        progressTotal_ > 0 ? static_cast<int>(std::min<size_t>(100, progressDone_ * 100 / progressTotal_)) : 0;
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, centerY + metrics.verticalSpacing,
                             pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                        state_ == State::Converting ? static_cast<int>(progressDone_) : percent, 100);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state_ == State::Complete) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, tr(STR_WORD_LENS_PACK_INSTALLED), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, I18N.get(errorMessage_), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}

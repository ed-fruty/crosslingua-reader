#include "modules/lingua/wordlens/WordLensService.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "modules/lingua/activities/LanguagePickerActivity.h"
#include "modules/lingua/wordlens/LensReader.h"
#include "modules/lingua/wordlens/WordLensGlosser.h"
#include "modules/lingua/wordlens/WordLensLevels.h"
#include "modules/lingua/wordlens/WordLensStorage.h"

namespace lingua::wordlens {
namespace {

// "en-US" -> "en", "zh-CN" -> "zh", "UK" -> "uk".
void primarySubtag(const char* code, char* out, const size_t outSize) {
  size_t n = 0;
  for (; code && code[n] && code[n] != '-' && code[n] != '_' && n + 1 < outSize; n++) {
    const char c = code[n];
    out[n] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 0x20) : c;
  }
  out[n] = '\0';
}

// dc:language is often a three-letter ISO 639-2 code (Calibre writes "eng"); packs are keyed by
// ISO 639-1. Covers every pack source language plus the hint languages packs exist for.
void toIso6391(char* code, const size_t size) {
  static constexpr const char* MAP[][2] = {{"eng", "en"}, {"fra", "fr"}, {"fre", "fr"}, {"deu", "de"}, {"ger", "de"},
                                           {"spa", "es"}, {"ita", "it"}, {"por", "pt"}, {"rus", "ru"}, {"ukr", "uk"},
                                           {"vie", "vi"}, {"hun", "hu"}, {"ara", "ar"}, {"zho", "zh"}, {"chi", "zh"}};
  for (const auto& m : MAP) {
    if (std::strcmp(code, m[0]) == 0) {
      std::snprintf(code, size, "%s", m[1]);
      return;
    }
  }
}

using LanguageCode = WordLensService::LanguageCode;

bool hasTarget(const LanguageCode* list, const size_t count, const char* code) {
  for (size_t i = 0; i < count; i++) {
    if (std::strcmp(list[i], code) == 0) return true;
  }
  return false;
}

void addTarget(LanguageCode* list, size_t& count, const size_t max, const char* code, const size_t len) {
  if (len == 0 || len >= sizeof(LanguageCode) || count >= max) return;
  char c[sizeof(LanguageCode)];
  std::memcpy(c, code, len);
  c[len] = '\0';
  if (!hasTarget(list, count, c)) std::memcpy(list[count++], c, sizeof(LanguageCode));
}

// Targets a cached manifest lists for `source`.
void manifestTargets(const char* path, const char* source, LanguageCode* list, size_t& count, const size_t max) {
  HalFile file;
  if (!Storage.exists(path) || !Storage.openFileForRead("WLENS", path, file)) return;
  JsonDocument filter;
  filter["packs"][0]["source"] = true;
  filter["packs"][0]["target"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, file, DeserializationOption::Filter(filter))) return;
  for (JsonObject p : doc["packs"].as<JsonArray>()) {
    const char* s = p["source"] | "";
    const char* t = p["target"] | "";
    if (std::strcmp(s, source) == 0) addTarget(list, count, max, t, std::strlen(t));
  }
}

// The primary subtag of a LanguagePickerActivity::LANGUAGES index; empty when out of range.
void pickerLanguage(const uint8_t index, char* out, const size_t outSize) {
  if (index < LanguagePickerActivity::NUM_LANGUAGES) {
    primarySubtag(LanguagePickerActivity::LANGUAGES[index].code, out, outSize);
  } else {
    out[0] = '\0';
  }
}

bool keepEmptyStrips() { return SETTINGS.wordLensEmptyStrips != CrossPointSettings::WORD_LENS_HIDE_EMPTY_STRIPS; }

bool wordLensModeActive() { return SETTINGS.translationDisplayMode == CrossPointSettings::LINGUA_WORD_LENS; }

}  // namespace

struct WordLensService::OpenPack {
  HalLensSource source;
  LensReader reader;
  WordLensGlosser glosser;
  char sourceLang[8] = {};
  char targetLang[8] = {};
  uint8_t level = 0;
  bool keepEmptyStrips = true;
};

WordLensService& WordLensService::instance() {
  static WordLensService service;
  return service;
}

WordLensService::WordLensService() = default;
WordLensService::~WordLensService() = default;

const char* WordLensService::languageName(const char* code) {
  const size_t len = std::strlen(code);
  for (int i = 0; i < LanguagePickerActivity::NUM_LANGUAGES; i++) {
    const char* c = LanguagePickerActivity::LANGUAGES[i].code;
    if (std::strncmp(c, code, len) == 0 && (c[len] == '\0' || c[len] == '-')) {
      return LanguagePickerActivity::LANGUAGES[i].name;
    }
  }
  return code;
}

size_t WordLensService::availableTargets(const char* source, LanguageCode* out, const size_t max) {
  size_t count = 0;
  if (!source || source[0] == '\0') return 0;
  // Installed: "word-lens-<source>-<target>.lens".
  char prefix[24];
  const int prefixLen = std::snprintf(prefix, sizeof(prefix), "word-lens-%s-", source);
  if (prefixLen > 0 && Storage.exists(PACK_DIR)) {
    for (const String& name : Storage.listFiles(PACK_DIR, 64)) {
      const char* n = name.c_str();
      const size_t len = name.length();
      if (len <= static_cast<size_t>(prefixLen) + 5 || std::strncmp(n, prefix, prefixLen) != 0 ||
          std::strcmp(n + len - 5, ".lens") != 0) {
        continue;
      }
      addTarget(out, count, max, n + prefixLen, len - prefixLen - 5);
    }
  }
  manifestTargets(MANIFEST_PATH, source, out, count, max);
  manifestTargets(READEST_MANIFEST_PATH, source, out, count, max);
  // Insertion sort: arrays are not assignable, and there are a handful.
  for (size_t i = 1; i < count; i++) {
    LanguageCode key;
    std::memcpy(key, out[i], sizeof(key));
    size_t j = i;
    for (; j > 0 && std::strcmp(out[j - 1], key) > 0; j--) std::memcpy(out[j], out[j - 1], sizeof(key));
    std::memcpy(out[j], key, sizeof(key));
  }
  return count;
}

void WordLensService::resolveTarget(char* out, const size_t outSize) const {
  LanguageCode targets[MAX_TARGETS];
  const size_t count = availableTargets(wantedSource_, targets, MAX_TARGETS);
  char translator[8];
  pickerLanguage(SETTINGS.translationLanguage, translator, sizeof(translator));
  char ui[8];
  primarySubtag(LANGUAGE_CODES[SETTINGS.language < getLanguageCount() ? SETTINGS.language : 0], ui, sizeof(ui));
  const char* chosen = nullptr;
  if (SETTINGS.wordLensHint[0] != '\0' && hasTarget(targets, count, SETTINGS.wordLensHint)) {
    chosen = SETTINGS.wordLensHint;
  } else if (translator[0] != '\0' && hasTarget(targets, count, translator)) {
    chosen = translator;
  } else if (hasTarget(targets, count, ui)) {
    chosen = ui;
  } else {
    for (size_t i = 0; i < count && !chosen; i++) {
      if (isInstalled(wantedSource_, targets[i])) chosen = targets[i];
    }
  }
  // Nothing offered for this source: name the pack the translator settings point at.
  if (!chosen) chosen = SETTINGS.wordLensHint[0] != '\0' ? SETTINGS.wordLensHint : translator[0] ? translator : ui;
  std::snprintf(out, outSize, "%s", chosen);
}

void WordLensService::sourceLanguage(const std::string& bookLanguage, char* out, const size_t outSize) {
  if (!bookLanguage.empty()) {
    primarySubtag(bookLanguage.c_str(), out, outSize);
    toIso6391(out, outSize);
    if (out[0] != '\0') return;
  }
  if (SETTINGS.sourceTranslationLanguage < LanguagePickerActivity::NUM_LANGUAGES) {
    primarySubtag(LanguagePickerActivity::LANGUAGES[SETTINGS.sourceTranslationLanguage].code, out, outSize);
    return;
  }
  out[0] = '\0';
}

bool WordLensService::isInstalled(const char* source, const char* target) {
  char path[64];
  packPath(source, target, path, sizeof(path));
  return Storage.exists(path);
}

const char* WordLensService::wantedTarget() const {
  if (!memoValid_ || std::strcmp(memoSource_, wantedSource_) != 0 ||
      std::strcmp(memoHint_, SETTINGS.wordLensHint) != 0 || memoTranslator_ != SETTINGS.translationLanguage ||
      memoUi_ != SETTINGS.language) {
    resolveTarget(wantedTarget_, sizeof(wantedTarget_));
    std::memcpy(memoSource_, wantedSource_, sizeof(memoSource_));
    std::snprintf(memoHint_, sizeof(memoHint_), "%s", SETTINGS.wordLensHint);
    memoTranslator_ = SETTINGS.translationLanguage;
    memoUi_ = SETTINGS.language;
    memoValid_ = true;
  }
  return wantedTarget_;
}

bool WordLensService::openPairMatches() const {
  if (!open_ || wantedSource_[0] == '\0') return false;
  return std::strcmp(open_->sourceLang, wantedSource_) == 0 && std::strcmp(open_->targetLang, wantedTarget()) == 0;
}

bool WordLensService::openMatches() const {
  return openPairMatches() && open_->level == SETTINGS.wordLensLevel && open_->keepEmptyStrips == keepEmptyStrips();
}

uint32_t WordLensService::cacheKey() const {
  if (!wordLensModeActive()) return 0;
  if (openMatches()) return wordLensCacheKey(open_->reader.header().contentHash, open_->level, open_->keepEmptyStrips);
  // No matching pack open (yet): a key of the wanted pair, so a section laid out without glosses
  // is rebuilt once the pack opens.
  char pair[20];
  const int n = std::snprintf(pair, sizeof(pair), "%s-%s", wantedSource_, wantedTarget());
  return wordLensCacheKey(fnv1a32(pair, n > 0 ? static_cast<size_t>(n) : 0), SETTINGS.wordLensLevel | 0x80,
                          keepEmptyStrips());
}

WordLensGlossSource* WordLensService::glossSource() { return glosser(); }

WordLensGlosser* WordLensService::glosser() {
  return wordLensModeActive() && openMatches() ? &open_->glosser : nullptr;
}

WordLensService::State WordLensService::state() const {
  if (!wordLensModeActive()) return State::Inactive;
  if (wantedSource_[0] == '\0') return State::NoLanguage;
  if (openPairMatches()) return State::Ready;
  if (!isInstalled(wantedSource_, wantedTarget())) return State::NotInstalled;
  // Installed but not open yet (it opens with the next section) or not openable.
  return lastState_ == State::OpenFailed ? State::OpenFailed : State::Ready;
}

void WordLensService::close() {
  if (open_) {
    open_->reader.close();
    open_->source.close();
    open_.reset();
  }
}

bool WordLensService::sync(const std::string& bookLanguage) {
  const uint32_t keyBefore = cacheKey();
  const WordLensGlossSource* sourceBefore = glossSource();
  char wanted[8];
  sourceLanguage(bookLanguage, wanted, sizeof(wanted));
  std::memcpy(wantedSource_, wanted, sizeof(wantedSource_));

  if (!wordLensModeActive()) {
    close();
    lastState_ = State::Inactive;
  } else if (wantedSource_[0] == '\0') {
    close();
    lastState_ = State::NoLanguage;
  } else if (!openMatches()) {
    // The memo re-reads the card here: sync() runs once per section, and a pack may have been
    // installed or deleted since.
    memoValid_ = false;
    char target[8];
    std::snprintf(target, sizeof(target), "%s", wantedTarget());
    // Same pair at another level or strip choice: keep the pack, only the configuration moves.
    if (openPairMatches()) {
      open_->level = SETTINGS.wordLensLevel;
      open_->keepEmptyStrips = keepEmptyStrips();
      open_->glosser.configure(&open_->reader, open_->level, open_->keepEmptyStrips);
      lastState_ = State::Ready;
    } else {
      close();
      char path[64];
      packPath(wantedSource_, target, path, sizeof(path));
      if (!Storage.exists(path)) {
        lastState_ = State::NotInstalled;
      } else {
        auto pack = makeUniqueNoThrow<OpenPack>();
        if (!pack) {
          LOG_ERR("WLENS", "OOM: Word Lens pack");
          lastState_ = State::OpenFailed;
        } else if (!pack->source.open(path) || !pack->reader.open(&pack->source)) {
          LOG_ERR("WLENS", "Cannot open %s", path);
          lastState_ = State::OpenFailed;
        } else {
          std::memcpy(pack->sourceLang, wantedSource_, sizeof(pack->sourceLang));
          std::memcpy(pack->targetLang, target, sizeof(pack->targetLang));
          pack->level = SETTINGS.wordLensLevel;
          pack->keepEmptyStrips = keepEmptyStrips();
          pack->glosser.configure(&pack->reader, pack->level, pack->keepEmptyStrips);
          LOG_INF("WLENS", "Opened %s (%u records, level %u)", path,
                  static_cast<unsigned>(pack->reader.header().recordCount), static_cast<unsigned>(pack->level));
          open_ = std::move(pack);
          lastState_ = State::Ready;
        }
      }
    }
  }
  return cacheKey() != keyBefore || glossSource() != sourceBefore;
}

}  // namespace lingua::wordlens

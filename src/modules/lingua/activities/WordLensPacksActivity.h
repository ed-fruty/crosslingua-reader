#pragma once

#include <I18n.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

namespace lingua::wordlens {
class LensConverter;
class HalLensFs;
}  // namespace lingua::wordlens

// Release assets: every CrossLingua firmware release carries the Word Lens packs and their manifest
// (.github/workflows/release.yml). GitHub's "latest" URL follows the newest non-prerelease release
// and redirects to objects.githubusercontent.com.
#ifndef WORD_LENS_RELEASE_BASE
#define WORD_LENS_RELEASE_BASE "https://github.com/ed-fruty/crosslingua-reader/releases/latest/download/"
#endif
// Fallback when the release cannot be reached: Readest's CDN, which serves the JSON packs our
// .lens files are made from (no en-uk there). The device converts them itself (LensBuilder).
#ifndef WORD_LENS_READEST_CDN
#define WORD_LENS_READEST_CDN "https://cdn.readest.com/wordlens/"
#endif

// Lingua > Word Lens > Gloss Pack: list the packs the manifest offers, download (with progress and
// a sha256 check) or delete them. Launched by the reader after it has released the book, like the
// translation flows; it returns to the reader through a clean reboot, the same Wi-Fi teardown and
// heap hygiene the font downloader applies.
//
// A Readest JSON pack is converted with Wi-Fi switched off (the download is already on SD), in
// small steps from loop() with a progress bar and Back to cancel. Wi-Fi is brought back through the
// Wi-Fi screen when another pack is picked afterwards.
class WordLensPacksActivity final : public UiListActivity {
 public:
  WordLensPacksActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string epubPath,
                        std::string bookLanguage);
  ~WordLensPacksActivity() override;

  void onEnter() override;
  void onExit() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state_ != State::List; }
  bool skipLoopDelay() override { return true; }

 private:
  enum class State : uint8_t { WifiSelection, Loading, List, Downloading, Converting, Complete, Error };

  struct Pack {
    char source[8] = {};
    char target[8] = {};
    char file[48] = {};
    char sha256[65] = {};
    uint32_t bytes = 0;
    // From Readest's CDN: a JSON pack the device converts after downloading.
    bool fromReadest = false;
    bool installed = false;
  };

  int listCount() const override { return static_cast<int>(packs_.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onBackButton() override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

  void startWifi();
  void onWifiSelectionComplete(bool success);
  void stopWifi();
  bool loadManifest();
  bool parseManifest(const char* path, bool readest);
  void refreshInstalled();
  void rebuildRows();
  void download(Pack& pack);
  bool downloadLens(const Pack& pack, const char* destination);
  bool downloadJson(const Pack& pack);
  bool startConversion();
  void stepConversion();
  void endConversion(bool ok);
  void finishDownload(bool ok);
  void deletePack(const Pack& pack);
  void pollCancel();
  void returnToReader();
  static bool sha256Matches(const char* path, const char* expectedHex);
  void setError(StrId message);

  std::string epubPath_;
  char bookSource_[8] = {};
  State state_ = State::WifiSelection;
  bool wifiUsed_ = false;
  // Wi-Fi is up and joined; false again once a conversion switched it off.
  bool wifiActive_ = false;
  bool cancelRequested_ = false;
  bool swallowBackRelease_ = false;
  // The pack to download once the Wi-Fi screen comes back connected, or -1.
  int pendingPack_ = -1;
  std::unique_ptr<lingua::wordlens::HalLensFs> lensFs_;
  std::unique_ptr<lingua::wordlens::LensConverter> converter_;
  std::string stagedPath_;
  std::vector<Pack> packs_;
  std::vector<std::string> rowLabels_;
  std::vector<freeink::ui::ListItem> rowItems_;
  bool rowsDirty_ = true;
  int activePack_ = -1;
  size_t progressDone_ = 0;
  size_t progressTotal_ = 0;
  StrId errorMessage_ = StrId::STR_WORD_LENS_PACK_FAILED;
};

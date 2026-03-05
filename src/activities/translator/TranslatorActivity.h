#pragma once
#include <functional>
#include <string>

#include "../ActivityWithSubactivity.h"
#include "translator/EpubTranslator.h"

/**
 * Full-screen activity that translates an EPUB book paragraph-by-paragraph.
 *
 * Flow:
 *  1. Show language picker (subactivity)
 *  2. Connect to WiFi (subactivity)
 *  3. Run translation in background FreeRTOS task, show progress
 *  4. Show result and wait for button press → call onDone()
 */
class TranslatorActivity final : public ActivityWithSubactivity {
 public:
  enum State {
    LANG_SELECTION,
    WIFI_SELECTION,
    TRANSLATING,
    DONE,
    FAILED,
    CANCELLED,
  };

  explicit TranslatorActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& epubPath,
                              const std::function<void()>& onDone)
      : ActivityWithSubactivity("Translator", renderer, mappedInput), epubPath(epubPath), onDone(onDone) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == TRANSLATING || state == WIFI_SELECTION; }

 private:
  std::string epubPath;
  std::string outputPath;
  std::string targetLangCode;
  std::string targetLangName;
  State state = LANG_SELECTION;
  EpubTranslator translator;
  EpubTranslator::Progress lastProgress;

  const std::function<void()> onDone;

  void onLangSelected(const char* code, const char* name);
  void onWifiConnected(bool success);
  void startTranslation();
  std::string buildOutputPath() const;
};

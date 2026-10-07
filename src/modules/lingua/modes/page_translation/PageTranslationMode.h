#pragma once
#include "modules/lingua/modes/common/LinguaMode.h"
#include "modules/lingua/modes/page_translation/PageTranslationOverlay.h"
class PageTranslationMode final : public LinguaMode {
 public:
  CrossPointSettings::LINGUA_MODE id() const override { return CrossPointSettings::LINGUA_PAGE_TRANSLATION; }
  static constexpr LinguaLayout layoutValue() { return LinguaLayout::OriginalOnly; }
  LinguaLayout layout() const override { return layoutValue(); }

  bool handleInput(MappedInputManager& input) { return overlay_.handleInput(input); }
  bool active() const { return overlay_.isActive(); }
  void open() { overlay_.open(); }
  void render(GfxRenderer& renderer, const Page& page, int fontId, int translationFontId, int xOffset, int yOffset,
              int viewportWidth, int viewportHeight) {
    overlay_.render(renderer, page, fontId, translationFontId, xOffset, yOffset, viewportWidth, viewportHeight);
  }
  void collectGlyphText(const Page& page, std::string& out) { overlay_.collectPageGlyphText(page, out); }
  void setTranslationSource(const std::string& htmlPath, const std::string& cachePath,
                            const std::string& bookPrimaryLang) {
    overlay_.setTranslationSource(htmlPath, cachePath, bookPrimaryLang);
  }
  void onPageChanged() { overlay_.onPageChanged(); }
  void onSectionChanged() { overlay_.onSectionChanged(); }

 private:
  PageTranslationOverlay overlay_;
};

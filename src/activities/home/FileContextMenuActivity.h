#pragma once
#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Small long-press context menu for a file entry in the Books browser.
//
// Presents up to two actions in a centered popup (mirroring ConfirmationActivity's
// look): Delete (always) and Pre-Translation (only when the entry is an EPUB).
// The chosen action is handed back to FileBrowserActivity via a MenuResult whose
// `action` field holds the selected Choice; a Back-out finishes cancelled.
class FileContextMenuActivity final : public Activity {
 public:
  enum class Choice : uint8_t {
    DELETE = 0,
    PRE_TRANSLATION = 1,
  };

  FileContextMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                          bool showPreTranslation);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  std::string title;
  std::string safeTitle;
  bool showPreTranslation;
  OptionPopup menuPopup;
};

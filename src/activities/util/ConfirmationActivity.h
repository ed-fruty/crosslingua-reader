#pragma once
#include <functional>
#include <string>

#include "../Activity.h"
#include "fontIds.h"

/**
 * Reusable yes/no confirmation dialog.
 *
 * Adapted from reader-cross-point-1.3's ConfirmationActivity: same centered
 * heading/body layout and Cancel/Confirm hints, but wired to our callback-based
 * navigation (onConfirm/onCancel) instead of the ActivityResult system. Intended
 * to run as a subactivity; the callbacks typically perform the action and then
 * exitActivity() on the parent.
 */
class ConfirmationActivity : public Activity {
 private:
  // Input data
  std::string heading;
  std::string body;

  const std::function<void()> onConfirm;
  const std::function<void()> onCancel;

  const int margin = 20;
  const int spacing = 30;
  const int fontId = UI_10_FONT_ID;

  std::string safeHeading;
  std::string safeBody;
  int startY = 0;
  int lineHeight = 0;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, const std::function<void()>& onConfirm,
                       const std::function<void()>& onCancel);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
};

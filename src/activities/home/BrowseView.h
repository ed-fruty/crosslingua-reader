#pragma once

#include <functional>
#include <memory>
#include <string>

class FileBrowserActivity;
class GfxRenderer;
class MappedInputManager;
class OptionPopup;
struct ActivityResult;

// CrossLingua: "Browse Files" has two views of the same folders: upstream's list
// (FileBrowserActivity) and the fork's cover grid (BookShelfActivity). The last
// used view is remembered on the SD card, without a settings entry. Upstream files
// reach this only through one-line `// CrossLingua:` hooks; FileBrowserActivity
// declares this struct a friend so the hooks and the cover view can reuse its own
// Open/Delete/Rename code instead of copying it.
struct BrowseView {
  // Entry long-press menu actions. Both views build the menu here, so they
  // offer the same items: Open/Delete/Rename for a file, Open/Delete for a
  // folder, then the switch to the other view. An empty folder offers only
  // the switch.
  enum class Action { Open, Delete, Rename, SwitchView };
  static void showMenu(OptionPopup& popup, bool hasEntry, bool isDirectory, bool inCoverView,
                       std::function<void(Action)> onAction);

  // Remembered view (default: list, as upstream).
  static bool coversRemembered();

  // Replaces the current activity with the other view at `folder`, selecting
  // `entryName` (no trailing '/') when that view lists it.
  static void switchToList(GfxRenderer& renderer, MappedInputManager& input, const std::string& folder,
                           const std::string& entryName, bool isDirectory);
  static void switchToCovers(GfxRenderer& renderer, MappedInputManager& input, const std::string& folder,
                             const std::string& entryName);

  // --- hooks in upstream code ----------------------------------------------
  // ActivityManager::goToFileBrowser: opens the cover view when it is the
  // remembered one. False keeps upstream's list (also on OOM).
  static bool openRemembered(GfxRenderer& renderer, MappedInputManager& input, const std::string& path);
  // FileBrowserActivity::showEntryActions: the shared menu. False leaves the
  // call to upstream (firmware picker mode).
  static bool showListActions(FileBrowserActivity& list);
  // FileBrowserActivity::handleCustomInput: touch long-press in an empty
  // folder, which has no row to carry it.
  static bool handleListInput(FileBrowserActivity& list);
  // FileBrowserActivity::onEnter: selects the entry a view switch carried over.
  static void restoreListSelection(FileBrowserActivity& list);

  // --- cover view Delete/Rename through the list's own code ----------------
  // Starts upstream's Delete (confirmation) or Rename (keyboard) flow for
  // `entryName` in `folder` on a FileBrowserActivity that is entered but never
  // shown. The dialog is pushed over the current activity, which must install
  // a result handler that calls finishListFileAction. Returns nullptr when
  // nothing was pushed.
  static std::unique_ptr<FileBrowserActivity> beginListFileAction(GfxRenderer& renderer, MappedInputManager& input,
                                                                  const std::string& folder,
                                                                  const std::string& entryName, bool isDirectory,
                                                                  bool rename);
  // Runs upstream's result handler (delete/rename and their side effects),
  // releases `list`, and returns the entry name the list would now select
  // (no trailing '/'; empty when the folder is empty).
  static std::string finishListFileAction(std::unique_ptr<FileBrowserActivity>& list, const ActivityResult& result);
};

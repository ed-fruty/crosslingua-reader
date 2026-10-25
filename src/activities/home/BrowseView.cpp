#include "BrowseView.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PersistableStore.h>

#include <array>

#include "BookShelfActivity.h"
#include "FileBrowserActivity.h"
#include "activities/ActivityManager.h"
#include "components/OptionPopup.h"

namespace {
constexpr const char* VIEW_FILE = "/.crosspoint/browse_view.json";

enum class Remembered : uint8_t { Unknown, List, Covers };
Remembered remembered = Remembered::Unknown;  // read once per boot, then cached

// Entry the list should select after a switch (with '/' for folders) and its
// folder; empty otherwise. Consumed by the next FileBrowserActivity::onEnter and
// applied only when that list shows the same folder.
std::string pendingListSelection;
std::string pendingListFolder;

void remember(const bool covers) {
  const Remembered next = covers ? Remembered::Covers : Remembered::List;
  if (remembered == next) return;
  remembered = next;
  JsonDocument doc;
  doc["covers"] = covers;
  if (!PersistableStoreBase::writeDocToFile(VIEW_FILE, doc)) LOG_ERR("BVIEW", "Failed to save browse view");
}

std::string withoutTrailingSlash(const std::string& entry) {
  if (!entry.empty() && entry.back() == '/') return entry.substr(0, entry.size() - 1);
  return entry;
}

// The list entry at `index` as the name both views share.
std::string nameAt(const std::vector<std::string>& files, const int index) {
  if (index < 0 || index >= static_cast<int>(files.size())) return {};
  return withoutTrailingSlash(files[index]);
}
}  // namespace

void BrowseView::showMenu(OptionPopup& popup, const bool hasEntry, const bool isDirectory, const bool inCoverView,
                          std::function<void(Action)> onAction) {
  StrId ids[4];
  std::array<Action, 4> actions{};
  int count = 0;
  const auto add = [&](const StrId id, const Action action) {
    ids[count] = id;
    actions[count] = action;
    ++count;
  };
  if (hasEntry) {
    add(StrId::STR_OPEN, Action::Open);
    add(StrId::STR_DELETE, Action::Delete);
    if (!isDirectory) add(StrId::STR_RENAME, Action::Rename);
  }
  add(inCoverView ? StrId::STR_LIST_VIEW : StrId::STR_COVERS_VIEW, Action::SwitchView);

  // Mirrors upstream's FILE_OPTIONS / DIRECTORY_OPTIONS in FileBrowserActivity::showEntryActions,
  // which this menu replaces: re-check them on every upstream merge.
  // Same title as upstream's list menu. OptionPopup copies the labels.
  popup.show(StrId::STR_FILENAME, ids, count, 0, [onAction = std::move(onAction), actions, count](const int index) {
    if (index >= 0 && index < count) onAction(actions[index]);
  });
}

bool BrowseView::coversRemembered() {
  if (remembered == Remembered::Unknown) {
    JsonDocument doc;
    const bool covers = PersistableStoreBase::readDocFromFile(VIEW_FILE, doc) && (doc["covers"] | false);
    remembered = covers ? Remembered::Covers : Remembered::List;
  }
  return remembered == Remembered::Covers;
}

void BrowseView::switchToList(GfxRenderer& renderer, MappedInputManager& input, const std::string& folder,
                              const std::string& entryName, const bool isDirectory) {
  auto list = makeUniqueNoThrow<FileBrowserActivity>(renderer, input, folder);
  if (!list) {
    LOG_ERR("BVIEW", "OOM: list view");
    return;
  }
  remember(false);
  pendingListSelection = entryName.empty() ? std::string() : (isDirectory ? entryName + "/" : entryName);
  pendingListFolder = folder;
  activityManager.replaceActivity(std::move(list));
}

void BrowseView::switchToCovers(GfxRenderer& renderer, MappedInputManager& input, const std::string& folder,
                                const std::string& entryName) {
  auto covers = makeUniqueNoThrow<BookShelfActivity>(renderer, input, folder, entryName);
  if (!covers) {
    LOG_ERR("BVIEW", "OOM: cover view");
    return;
  }
  remember(true);
  activityManager.replaceActivity(std::move(covers));
}

bool BrowseView::openRemembered(GfxRenderer& renderer, MappedInputManager& input, const std::string& path) {
  if (!coversRemembered()) return false;
  auto covers = makeUniqueNoThrow<BookShelfActivity>(renderer, input, path);
  if (!covers) {
    LOG_ERR("BVIEW", "OOM: cover view, opening the list");
    return false;
  }
  activityManager.replaceActivity(std::move(covers));
  return true;
}

bool BrowseView::showListActions(FileBrowserActivity& list) {
  if (list.mode != FileBrowserActivity::Mode::Books || list.optionPopup.isActive()) return false;
  const int selected = list.nav.selected;
  const bool hasEntry = selected >= 0 && selected < list.listCount();
  if (!list.files.empty() && !hasEntry) return true;  // stale index; upstream ignores it too
  const bool isDirectory = hasEntry && list.files[selected].back() == '/';
  showMenu(list.optionPopup, hasEntry, isDirectory, /*inCoverView=*/false, [&list](const Action action) {
    switch (action) {
      case Action::Open:
        list.activateSelected();
        break;
      case Action::Delete:
        list.deleteSelected();
        break;
      case Action::Rename:
        list.startRename();
        break;
      case Action::SwitchView:
        switchToCovers(list.renderer, list.mappedInput, list.basepath, nameAt(list.files, list.nav.selected));
        break;
    }
  });
  list.requestUpdate();
  return true;
}

bool BrowseView::handleListInput(FileBrowserActivity& list) {
  if (list.mode != FileBrowserActivity::Mode::Books || !list.files.empty()) return false;
  int x = 0;
  int y = 0;
  if (!list.mappedInput.wasScreenLongPress(x, y)) return false;
  return showListActions(list);
}

void BrowseView::restoreListSelection(FileBrowserActivity& list) {
  if (pendingListSelection.empty()) return;
  if (list.mode == FileBrowserActivity::Mode::Books && list.basepath == pendingListFolder) {
    list.nav.selected = static_cast<int>(list.findEntry(pendingListSelection));
  }
  std::string().swap(pendingListSelection);
  std::string().swap(pendingListFolder);
}

std::unique_ptr<FileBrowserActivity> BrowseView::beginListFileAction(GfxRenderer& renderer, MappedInputManager& input,
                                                                     const std::string& folder,
                                                                     const std::string& entryName,
                                                                     const bool isDirectory, const bool rename) {
  auto list = makeUniqueNoThrow<FileBrowserActivity>(renderer, input, folder);
  if (!list) {
    LOG_ERR("BVIEW", "OOM: list file action");
    return nullptr;
  }
  // Upstream's own entry: allocates its name buffer and lists the folder.
  list->onEnter();
  const std::string entry = isDirectory ? entryName + "/" : entryName;
  const size_t index = list->findEntry(entry);
  if (index >= list->files.size() || list->files[index] != entry) {
    LOG_ERR("BVIEW", "Entry not in list view: %s", entry.c_str());
    list->onExit();
    return nullptr;
  }
  list->nav.selected = static_cast<int>(index);
  if (rename) {
    list->startRename();
  } else {
    list->deleteSelected();
  }
  // Upstream pushed its dialog with a handler on `list`; the caller runs it
  // through finishListFileAction once the dialog returns to the caller.
  if (!list->resultHandler) {
    list->onExit();
    return nullptr;
  }
  return list;
}

std::string BrowseView::finishListFileAction(std::unique_ptr<FileBrowserActivity>& list, const ActivityResult& result) {
  if (!list) return {};
  auto handler = std::move(list->resultHandler);
  list->resultHandler = nullptr;
  if (handler) handler(result);
  // Upstream's flow is one dialog today. A follow-up dialog would be pushed with a
  // handler on `list`, which is released below, and its result would be lost.
  if (list->resultHandler) LOG_ERR("BVIEW", "List file action chained a second dialog; its result is dropped");
  std::string selection = nameAt(list->files, list->nav.selected);
  list->onExit();
  list.reset();
  return selection;
}

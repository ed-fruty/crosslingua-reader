#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

class MyLibraryActivity final : public ActivityWithSubactivity {
 private:
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  // Deletion: recursively delete a file or directory, clearing reading caches as it goes.
  bool removeDirFile(const std::string& fullPath);
  // Opens the delete-confirmation subactivity for the given entry.
  void promptDelete(const std::string& entry);

  // Delete confirmation is handled across loop() iterations to avoid deleting the
  // confirmation subactivity from inside its own callback (use-after-free).
  enum class PendingConfirm { None, Confirm, Cancel };
  volatile PendingConfirm pendingConfirm = PendingConfirm::None;
  std::string pendingDeletePath;

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path)>& onSelectBook,
                             std::string initialPath = "/")
      : ActivityWithSubactivity("MyLibrary", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
};

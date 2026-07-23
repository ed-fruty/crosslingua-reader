#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>  // for the held FontCacheManager::PrewarmScope member below

#include <cstdint>
#include <optional>

#include "BookmarkEntry.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "ProgressMapper.h"
#include "activities/Activity.h"
#include "translator/ModalOverlay.h"
#include "translator/TooltipOverlay.h"

// Defined in PreTranslationSubmenuActivity.h; forward-declared here so the reader
// header does not pull in the submenu (and its transitive) headers.
enum class PreTranslationResult : uint8_t;

class EpubReaderActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterTotalPageCount = 0;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  // Set when the Pre-Translation submenu changed SETTINGS.translationDisplayMode: the section must
  // re-layout under the new ReaderRenderSpec (translationMode is part of the section.bin cache key)
  // and the reader must land on the proportionally-equivalent page. It keeps the cached page count
  // alive past render()'s cacheLoaded reset (so a cache-complete re-switch still remaps) and forces
  // a full (blocking) build (so a cache-miss first switch remaps against the final count) -- a
  // windowed build can finalize after applyDeferredReposition()'s window, silently dropping the
  // reposition. See the PRE_TRANSLATION submenu-return handler and render().
  bool pendingModeReposition = false;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  // Consecutive page-load failures. Each failure drops the section and rebuilds on the next render,
  // which recovers a transiently corrupt cache; capped so a persistently bad page can't spin forever.
  uint8_t pageLoadRetryCount = 0;
  static constexpr uint8_t MAX_PAGE_LOAD_RETRIES = 3;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool showBookmarkMessage = false;
  // "No dictionary set" popup, shown when a lookup is triggered without a configured dictionary.
  bool showDictionaryMessage = false;
  unsigned long dictionaryMessageTime = 0UL;
  bool ignoreNextConfirmRelease = false;
  // Pre-Translation modal overlay (PT_MODAL mode). Opened by a long-press RELEASE on either side
  // button, detected in loop() before detectPageTurn: detecting the OPEN on the release (not
  // mid-hold) means the same release cannot also be consumed as a scroll by handleInput(), and
  // returning after open() suppresses the page-turn / chapter-skip / orientation long-press that
  // would otherwise fire on that release -- so no ignore-next-release latch is needed.
  ModalOverlay modalOverlay;
  // Pre-Translation tooltip overlay (PT_TOOLTIP mode). Owns its configured nav buttons for
  // per-sentence stepping and its own long-press page-turn; see loop()'s tooltip input block.
  TooltipOverlay tooltipOverlay;
  // Retained reader-font glyph prewarm for an ACTIVE translation overlay. Built ONCE (wipe + scan +
  // prewarm) when an overlay opens or the page under it turns, then HELD across every sentence-step /
  // modal-scroll so those steps reuse the warm page buffer instead of re-wiping and re-decoding the
  // whole page on demand each press (that on-demand path was ~10x slower -- see renderOverlayFrame()).
  // Torn down when the overlay closes (normal branch of renderContents) and in onExit(); the scope's
  // dtor clears the decompressor cache. Reuse is gated on the page identity it was built for AND the
  // cache generation: any intervening clearCache() (a dictionary sub-activity, the next normal render)
  // bumps FontCacheManager::cacheGeneration(), forcing a rebuild instead of reusing a wiped cache.
  std::optional<FontCacheManager::PrewarmScope> overlayPrewarm_;
  int overlayPrewarmSpine_ = -1;
  int overlayPrewarmPage_ = -1;
  int overlayPrewarmFontId_ = -1;
  uint32_t overlayPrewarmGen_ = 0;
  // Shown when a PT_MODAL long-press opens the overlay on a page that has NO translated
  // paragraphs: the overlay refuses (clears its active flag in render()), and the reader surfaces
  // this toast instead of the previous silent no-op. Timed out in loop() like the other toasts.
  bool showModalNoTranslationToast = false;
  unsigned long modalNoTranslationToastTime = 0UL;
  // Pre-Translation: shown when the current chapter has no translated HTML but the user picked a
  // non-Normal display mode. render() detects this on entry to each chapter and lays the chapter out
  // in Normal for THAT chapter only (per-chapter fallback -- the display-mode setting is preserved),
  // toasting so the switch isn't silent.
  bool showingAutoFallbackToast = false;
  unsigned long autoFallbackToastTime = 0UL;
  bool currentPageBookmarked = false;
  bool bookmarkRemoved = false;  // true when last toggle removed (controls popup text)
  std::vector<BookmarkEntry> cachedBookmarks;
  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  unsigned long bookmarkMessageTime = 0UL;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with IDENTICAL layout parameters to the pages already rendered (a mismatch
  // would paginate differently than the partial being extended). 0 = no render yet.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() doesn't retry (and log) every
  // tick; the blocking extension in render() remains the fallback past the watermark.
  bool partialRebuildStartFailed = false;

  // Last position persisted by render()'s saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders (menu/bookmark/screenshot).
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(Page& page, int orientedMarginTop, int orientedMarginRight, int orientedMarginBottom,
                      int orientedMarginLeft);
  // Fork-parity render path for a page with an active translation overlay (PT_TOOLTIP / PT_MODAL):
  // page + status bar + overlay composited into ONE BW frame, a single refresh, and (when the page
  // is visible, i.e. not under the modal) the grayscale AA pass. Avoids the second slow refresh the
  // old overlay path did on every sentence step / scroll.
  void renderOverlayFrame(Page& page, int fontId, int orientedMarginTop, int orientedMarginRight,
                          int orientedMarginBottom, int orientedMarginLeft);
  void renderStatusBar() const;
  // Pages laid out per incremental-build pump: on the render path (catching up to the page
  // being shown) and per loop() tick (background build of a large chapter). Kept small so a
  // background build chunk never noticeably delays input or a pending render.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  // How many pages to keep laid out ahead of the reader for a still-building section. A page
  // turn is ~1s on e-ink and a page builds in ~30ms, so the reader can't out-click the builder
  // -- a tiny buffer is enough. The background build stops once the watermark is this far
  // ahead and resumes as the reader advances; building unbounded instead locked up input by
  // monopolizing the RenderLock. A giant single-spine book therefore never finalizes its .bin
  // in one sitting -- instant reopen comes from Section::suspendBuild() persisting the pages
  // already laid out as a partial file on exit/sleep.
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  // Reopening a partial does NOT immediately restart its extension build (a whole-chapter
  // re-layout from page 0 -- minutes of background CPU + SD writes on a giant spine, wasted
  // when the reader never crosses the watermark that session). Instead loop() starts it once
  // the reader is within this many pages of the watermark: at ~30s per page read and ~100-300ms
  // per page rebuilt, this margin gives the rebuild ample runway to catch up (and finalize)
  // before the reader arrives.
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the small look-ahead window so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  // No-op while the section is still building or when the pagination is unchanged (plain resume).
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void openDictionaryWordSelect();
  // Returns true if sync acted (launched, or surfaced a save error); false if it was a no-op
  // because no KOReader credentials are stored.
  bool launchKOReaderSync();
  // Tears down the reader (saveProgress -> release Epub + Section) and replaces it
  // with the chapter/book translator, modeled on launchKOReaderSync(): the ~65KB
  // freed lets wolfSSL complete the TLS handshake. The translator relaunches the
  // reader on exit via goToReader(). Called from the PRE_TRANSLATION result handler.
  void launchTranslation(PreTranslationResult kind);
  void applyOrientation(uint8_t orientation);
  void toggleAutoPageTurn(uint8_t selectedPageTurnOption);
  void pageTurn(bool isForwardTurn);
  void loadCachedBookmarks();
  void addBookmark();
  void updateBookmarkFlag();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false);
  void restoreSavedPosition();

  // Pre-Translation: triggered by Section when a chapter has no translation
  // but mode is non-Normal. Resets the global mode and queues a toast.
  void showAutoFallbackToast();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub)
      : Activity("EpubReader", renderer, mappedInput), epub(std::move(epub)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  // Full CPU speed + fast loop ticks while a section build runs: at the low-power
  // frequency a giant chapter's background rebuild stretches from ~40s to many
  // minutes, so the reader exits before it can finalize and the next open restarts
  // it from page 0. Reverts to normal power behavior the moment the build finishes.
  bool skipLoopDelay() override { return section && section->isBuilding(); }
  bool isReaderActivity() const override { return true; }
  ScreenshotInfo getScreenshotInfo() const override;
  CrossPointPosition getCurrentPosition() const;
};

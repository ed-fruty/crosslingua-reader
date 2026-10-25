#include "components/CoverGrid.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "components/CoverGridLayout.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"

namespace {
using covergrid::GRID_CELL_PADDING;
using covergrid::GRID_COLS;
using covergrid::GRID_ROWS;
using covergrid::GRID_TITLE_AREA;

// Rounded light-gray selection radii, matching the constants each theme draws its own tiles with.
constexpr int LYRA_SELECTION_RADIUS = 6;
constexpr int ROUNDED_RAFF_SELECTION_RADIUS = 20;  // RoundedRaffTheme kRowRadius

// Blit the cached cover thumb into a cell, letterboxed to preserve aspect. Returns false when no
// drawable BMP was available (0-byte negative-cache sentinel, unparseable header, or open failure)
// so the caller can fall back to a folder/placeholder glyph. `clearBehind` whitens the cover's own
// footprint first: drawBitmap skips white pixels, so on a black selection fill the cover would vanish.
bool drawCellCover(GfxRenderer& renderer, const std::string& thumbPath, int thumbX, int thumbY, int thumbWidth,
                   int thumbHeight, bool clearBehind) {
  if (thumbPath.empty()) return false;
  HalFile file;
  bool drew = false;
  if (Storage.openFileForRead("BSHELF", thumbPath, file)) {
    if (file.size() > 0) {
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() == BmpReaderError::Ok) {
        int coverX = thumbX;
        int coverY = thumbY;
        if (bitmap.getWidth() > 0 && bitmap.getHeight() > 0) {
          const float imgRatio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
          const float boxRatio = static_cast<float>(thumbWidth) / static_cast<float>(thumbHeight);
          if (imgRatio > boxRatio) {
            coverY = thumbY + (thumbHeight - static_cast<int>(thumbWidth / imgRatio)) / 2;
          } else {
            coverX = thumbX + (thumbWidth - static_cast<int>(thumbHeight * imgRatio)) / 2;
          }
          if (clearBehind) {
            // drawBitmap only scales down, never up.
            const float scale = std::min(1.0f, std::min(static_cast<float>(thumbWidth) / bitmap.getWidth(),
                                                        static_cast<float>(thumbHeight) / bitmap.getHeight()));
            renderer.fillRect(coverX, coverY, static_cast<int>(bitmap.getWidth() * scale),
                              static_cast<int>(bitmap.getHeight() * scale), false);
          }
        }
        renderer.drawBitmap(bitmap, coverX, coverY, thumbWidth, thumbHeight);
        drew = true;
      }
    }
    file.close();
  }
  return drew;
}

void drawFolderGlyph(GfxRenderer& renderer, int thumbX, int thumbY, int thumbWidth, int thumbHeight, bool state) {
  const int folderW = 80, bodyH = 50, tabW = 28, tabH = 12;
  const int folderX = thumbX + (thumbWidth - folderW) / 2;
  const int folderY = thumbY + (thumbHeight - (bodyH + tabH - 2)) / 2;
  renderer.drawRoundedRect(folderX, folderY, tabW, tabH, 2, 4, true, true, false, false, state);
  renderer.drawRoundedRect(folderX, folderY + tabH - 2, folderW, bodyH, 2, 6, state);
}

// Three centered dots: the placeholder for a cover whose thumbnail is still being generated.
void drawLoadingGlyph(GfxRenderer& renderer, int thumbX, int thumbY, int thumbWidth, int thumbHeight, bool state) {
  constexpr int dotSize = 8, dotGap = 10;
  const int totalW = dotSize * 3 + dotGap * 2;
  int dotX = thumbX + (thumbWidth - totalW) / 2;
  const int dotY = thumbY + (thumbHeight - dotSize) / 2;
  for (int i = 0; i < 3; i++) {
    renderer.fillRect(dotX, dotY, dotSize, dotSize, state);
    dotX += dotSize + dotGap;
  }
}

void fillSelectedCell(GfxRenderer& renderer, const CoverGridStyle& style, int cellX, int cellY, int cellWidth,
                      int cellHeight) {
  if (style.selectionRadius > 0) {
    renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, style.selectionRadius,
                             Color::LightGray);
  } else {
    renderer.fillRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4);
  }
}

// One cell, selected or not. Shared by the full-page paint and the single-cell repaint so the two
// can never drift in geometry or in what a selected cell looks like.
void drawCell(GfxRenderer& renderer, const CoverGridStyle& style, Rect rect, int index, int pageOffset, bool selected,
              const std::function<std::string(int)>& getTitle, const std::function<std::string(int)>& getThumbPath,
              const std::function<bool(int)>& isDirectory, const std::function<bool(int)>& isPending) {
  const int cellWidth = rect.width / GRID_COLS;
  const int cellHeight = rect.height / GRID_ROWS;
  const int thumbWidth = cellWidth - GRID_CELL_PADDING * 2;
  const int thumbHeight = cellHeight - GRID_CELL_PADDING * 2 - GRID_TITLE_AREA;

  const int gridIdx = index - pageOffset;
  const int cellX = rect.x + (gridIdx % GRID_COLS) * cellWidth;
  const int cellY = rect.y + (gridIdx / GRID_COLS) * cellHeight;

  if (selected) fillSelectedCell(renderer, style, cellX, cellY, cellWidth, cellHeight);

  // Black content everywhere except on a selected cell of a theme that inverts it.
  const bool inkState = !(selected && style.invertSelectedContent);

  const int thumbX = cellX + (cellWidth - thumbWidth) / 2;
  const int thumbY = cellY + GRID_CELL_PADDING;

  if (isDirectory(index)) {
    drawFolderGlyph(renderer, thumbX, thumbY, thumbWidth, thumbHeight, inkState);
  } else if (!drawCellCover(renderer, getThumbPath(index), thumbX, thumbY, thumbWidth, thumbHeight, !inkState)) {
    if (isPending(index)) {
      drawLoadingGlyph(renderer, thumbX, thumbY, thumbWidth, thumbHeight, inkState);
    } else {
      // A processed book without a drawable cover gets the same placeholder as the home screen. Its
      // white part needs a white ground on a black selection fill.
      if (!inkState) renderer.fillRect(thumbX, thumbY, thumbWidth, thumbHeight, false);
      BaseTheme::drawCoverPlaceholder(renderer, Rect{thumbX, thumbY, thumbWidth, thumbHeight});
    }
  }

  const std::string title = getTitle(index);
  const int titleY = thumbY + thumbHeight + 1;
  const int maxTitleWidth = cellWidth - GRID_CELL_PADDING * 2;
  const auto truncated = renderer.truncatedText(SMALL_FONT_ID, title.c_str(), maxTitleWidth);
  const int titleTextWidth = renderer.getTextWidth(SMALL_FONT_ID, truncated.c_str());
  const int titleX = cellX + (cellWidth - titleTextWidth) / 2;
  renderer.drawText(SMALL_FONT_ID, titleX, titleY, truncated.c_str(), inkState);
}
}  // namespace

CoverGridStyle activeCoverGridStyle() {
  // Lyra, its 3-covers variant and RoundedRaff paint a rounded light-gray selection and keep their
  // cell content black; every other theme uses the square black fill with inverted content.
  switch (SETTINGS.uiTheme) {
    case CrossPointSettings::LYRA:
    case CrossPointSettings::LYRA_3_COVERS:
      return CoverGridStyle{LYRA_SELECTION_RADIUS, /*invertSelectedContent=*/false};
    case CrossPointSettings::ROUNDEDRAFF:
      return CoverGridStyle{ROUNDED_RAFF_SELECTION_RADIUS, /*invertSelectedContent=*/false};
    default:
      return CoverGridStyle{};
  }
}

void drawCoverGrid(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int pageOffset,
                   const std::function<std::string(int)>& getTitle, const std::function<std::string(int)>& getThumbPath,
                   const std::function<bool(int)>& isDirectory, const std::function<bool(int)>& isPending) {
  const CoverGridStyle style = activeCoverGridStyle();
  const int pageEnd = std::min(pageOffset + GRID_COLS * GRID_ROWS, itemCount);
  for (int i = pageOffset; i < pageEnd; i++) {
    drawCell(renderer, style, rect, i, pageOffset, i == selectedIndex, getTitle, getThumbPath, isDirectory, isPending);
  }
}

void drawCoverGridSelection(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int pageOffset,
                            const std::function<std::string(int)>& getTitle,
                            const std::function<std::string(int)>& getThumbPath,
                            const std::function<bool(int)>& isDirectory, const std::function<bool(int)>& isPending) {
  if (selectedIndex < pageOffset || selectedIndex >= std::min(pageOffset + GRID_COLS * GRID_ROWS, itemCount)) return;
  drawCell(renderer, activeCoverGridStyle(), rect, selectedIndex, pageOffset, /*selected=*/true, getTitle, getThumbPath,
           isDirectory, isPending);
}

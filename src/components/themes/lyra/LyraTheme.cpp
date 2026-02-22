#include "LyraTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <cstdint>
#include <string>

#include "Battery.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

// Internal constants
namespace {
constexpr int batteryPercentSpacing = 4;
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
constexpr int topHintButtonY = 345;
}  // namespace

void LyraTheme::drawBatteryLeft(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Left aligned: icon on left, percentage on right (reader mode)
  const uint16_t percentage = battery.readPercentage();
  const int y = rect.y + 6;
  const int battWidth = LyraMetrics::values.batteryWidth;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    renderer.drawText(SMALL_FONT_ID, rect.x + batteryPercentSpacing + battWidth, rect.y, percentageText.c_str());
  }

  // Draw icon
  const int x = rect.x;
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rect.height - 1, x + battWidth - 3, y + rect.height - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rect.height - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rect.height - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rect.height - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rect.height - 5);

  // Draw bars
  if (percentage > 10) {
    renderer.fillRect(x + 2, y + 2, 3, rect.height - 4);
  }
  if (percentage > 40) {
    renderer.fillRect(x + 6, y + 2, 3, rect.height - 4);
  }
  if (percentage > 70) {
    renderer.fillRect(x + 10, y + 2, 3, rect.height - 4);
  }
}

void LyraTheme::drawBatteryRight(const GfxRenderer& renderer, Rect rect, const bool showPercentage) const {
  // Right aligned: percentage on left, icon on right (UI headers)
  const uint16_t percentage = battery.readPercentage();
  const int y = rect.y + 6;
  const int battWidth = LyraMetrics::values.batteryWidth;

  if (showPercentage) {
    const auto percentageText = std::to_string(percentage) + "%";
    const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, percentageText.c_str());
    // Clear the area where we're going to draw the text to prevent ghosting
    const auto textHeight = renderer.getTextHeight(SMALL_FONT_ID);
    renderer.fillRect(rect.x - textWidth - batteryPercentSpacing, rect.y, textWidth, textHeight, false);
    // Draw text to the left of the icon
    renderer.drawText(SMALL_FONT_ID, rect.x - textWidth - batteryPercentSpacing, rect.y, percentageText.c_str());
  }

  // Draw icon at rect.x
  const int x = rect.x;
  // Top line
  renderer.drawLine(x + 1, y, x + battWidth - 3, y);
  // Bottom line
  renderer.drawLine(x + 1, y + rect.height - 1, x + battWidth - 3, y + rect.height - 1);
  // Left line
  renderer.drawLine(x, y + 1, x, y + rect.height - 2);
  // Battery end
  renderer.drawLine(x + battWidth - 2, y + 1, x + battWidth - 2, y + rect.height - 2);
  renderer.drawPixel(x + battWidth - 1, y + 3);
  renderer.drawPixel(x + battWidth - 1, y + rect.height - 4);
  renderer.drawLine(x + battWidth - 0, y + 4, x + battWidth - 0, y + rect.height - 5);

  // Draw bars
  if (percentage > 10) {
    renderer.fillRect(x + 2, y + 2, 3, rect.height - 4);
  }
  if (percentage > 40) {
    renderer.fillRect(x + 6, y + 2, 3, rect.height - 4);
  }
  if (percentage > 70) {
    renderer.fillRect(x + 10, y + 2, 3, rect.height - 4);
  }
}

void LyraTheme::drawHeader(const GfxRenderer& renderer, Rect rect, const char* title) const {
  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);

  const bool showBatteryPercentage =
      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS;
  // Position icon at right edge, drawBatteryRight will place text to the left
  const int batteryX = rect.x + rect.width - 12 - LyraMetrics::values.batteryWidth;
  drawBatteryRight(renderer,
                   Rect{batteryX, rect.y + 5, LyraMetrics::values.batteryWidth, LyraMetrics::values.batteryHeight},
                   showBatteryPercentage);

  if (title) {
    auto truncatedTitle = renderer.truncatedText(
        UI_12_FONT_ID, title, rect.width - LyraMetrics::values.contentSidePadding * 2, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding,
                      rect.y + LyraMetrics::values.batteryBarHeight + 3, truncatedTitle.c_str(), true,
                      EpdFontFamily::BOLD);
    renderer.drawLine(rect.x, rect.y + rect.height - 3, rect.x + rect.width, rect.y + rect.height - 3, 3, true);
  }
}

void LyraTheme::drawTabBar(const GfxRenderer& renderer, Rect rect, const std::vector<TabInfo>& tabs,
                           bool selected) const {
  int currentX = rect.x + LyraMetrics::values.contentSidePadding;

  if (selected) {
    renderer.fillRectDither(rect.x, rect.y, rect.width, rect.height, Color::LightGray);
  }

  for (const auto& tab : tabs) {
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, tab.label, EpdFontFamily::REGULAR);

    if (tab.selected) {
      if (selected) {
        renderer.fillRoundedRect(currentX, rect.y + 1, textWidth + 2 * hPaddingInSelection, rect.height - 4,
                                 cornerRadius, Color::Black);
      } else {
        renderer.fillRectDither(currentX, rect.y, textWidth + 2 * hPaddingInSelection, rect.height - 3,
                                Color::LightGray);
        renderer.drawLine(currentX, rect.y + rect.height - 3, currentX + textWidth + 2 * hPaddingInSelection,
                          rect.y + rect.height - 3, 2, true);
      }
    }

    renderer.drawText(UI_10_FONT_ID, currentX + hPaddingInSelection, rect.y + 6, tab.label, !(tab.selected && selected),
                      EpdFontFamily::REGULAR);

    currentX += textWidth + LyraMetrics::values.tabSpacing + 2 * hPaddingInSelection;
  }

  renderer.drawLine(rect.x, rect.y + rect.height - 1, rect.x + rect.width, rect.y + rect.height - 1, true);
}

void LyraTheme::drawList(const GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                         const std::function<std::string(int index)>& rowTitle,
                         const std::function<std::string(int index)>& rowSubtitle,
                         const std::function<std::string(int index)>& rowIcon,
                         const std::function<std::string(int index)>& rowValue) const {
  int rowHeight =
      (rowSubtitle != nullptr) ? LyraMetrics::values.listWithSubtitleRowHeight : LyraMetrics::values.listRowHeight;
  int pageItems = rect.height / rowHeight;

  const int totalPages = (itemCount + pageItems - 1) / pageItems;
  if (totalPages > 1) {
    const int scrollAreaHeight = rect.height;

    // Draw scroll bar
    const int scrollBarHeight = (scrollAreaHeight * pageItems) / itemCount;
    const int currentPage = selectedIndex / pageItems;
    const int scrollBarY = rect.y + ((scrollAreaHeight - scrollBarHeight) * currentPage) / (totalPages - 1);
    const int scrollBarX = rect.x + rect.width - LyraMetrics::values.scrollBarRightOffset;
    renderer.drawLine(scrollBarX, rect.y, scrollBarX, rect.y + scrollAreaHeight, true);
    renderer.fillRect(scrollBarX - LyraMetrics::values.scrollBarWidth, scrollBarY, LyraMetrics::values.scrollBarWidth,
                      scrollBarHeight, true);
  }

  // Draw selection
  int contentWidth =
      rect.width -
      (totalPages > 1 ? (LyraMetrics::values.scrollBarWidth + LyraMetrics::values.scrollBarRightOffset) : 1);
  if (selectedIndex >= 0) {
    renderer.fillRoundedRect(LyraMetrics::values.contentSidePadding, rect.y + selectedIndex % pageItems * rowHeight,
                             contentWidth - LyraMetrics::values.contentSidePadding * 2, rowHeight, cornerRadius,
                             Color::LightGray);
  }

  // Draw all items
  const auto pageStartIndex = selectedIndex / pageItems * pageItems;
  for (int i = pageStartIndex; i < itemCount && i < pageStartIndex + pageItems; i++) {
    const int itemY = rect.y + (i % pageItems) * rowHeight;

    // Draw name
    int textWidth = contentWidth - LyraMetrics::values.contentSidePadding * 2 - hPaddingInSelection * 2 -
                    (rowValue != nullptr ? 60 : 0);  // TODO truncate according to value width?
    auto itemName = rowTitle(i);
    auto item = renderer.truncatedText(UI_10_FONT_ID, itemName.c_str(), textWidth);
    renderer.drawText(UI_10_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection * 2,
                      itemY + 6, item.c_str(), true);

    if (rowSubtitle != nullptr) {
      // Draw subtitle
      std::string subtitleText = rowSubtitle(i);
      auto subtitle = renderer.truncatedText(SMALL_FONT_ID, subtitleText.c_str(), textWidth);
      renderer.drawText(SMALL_FONT_ID, rect.x + LyraMetrics::values.contentSidePadding + hPaddingInSelection * 2,
                        itemY + 30, subtitle.c_str(), true);
    }

    if (rowValue != nullptr) {
      // Draw value
      std::string valueText = rowValue(i);
      if (!valueText.empty()) {
        const auto valueTextWidth = renderer.getTextWidth(UI_10_FONT_ID, valueText.c_str());

        if (i == selectedIndex) {
          renderer.fillRoundedRect(
              contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection * 2 - valueTextWidth, itemY,
              valueTextWidth + hPaddingInSelection * 2, rowHeight, cornerRadius, Color::Black);
        }

        renderer.drawText(UI_10_FONT_ID,
                          contentWidth - LyraMetrics::values.contentSidePadding - hPaddingInSelection - valueTextWidth,
                          itemY + 6, valueText.c_str(), i != selectedIndex);
      }
    }
  }
}

void LyraTheme::drawButtonHints(GfxRenderer& renderer, const char* btn1, const char* btn2, const char* btn3,
                                const char* btn4) const {
  const GfxRenderer::Orientation orig_orientation = renderer.getOrientation();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  const int pageHeight = renderer.getScreenHeight();
  constexpr int buttonWidth = 80;
  constexpr int smallButtonHeight = 15;
  constexpr int buttonHeight = LyraMetrics::values.buttonHintsHeight;
  constexpr int buttonY = LyraMetrics::values.buttonHintsHeight;  // Distance from bottom
  constexpr int textYOffset = 7;                                  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {58, 146, 254, 342};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    const int x = buttonPositions[i];
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      // Draw the filled background and border for a FULL-sized button
      renderer.fillRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, false);
      renderer.drawRoundedRect(x, pageHeight - buttonY, buttonWidth, buttonHeight, 1, cornerRadius, true, true, false,
                               false, true);
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      renderer.drawText(SMALL_FONT_ID, textX, pageHeight - buttonY + textYOffset, labels[i]);
    } else {
      // Draw the filled background and border for a SMALL-sized button
      renderer.fillRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, false);
      renderer.drawRoundedRect(x, pageHeight - smallButtonHeight, buttonWidth, smallButtonHeight, 1, cornerRadius, true,
                               true, false, false, true);
    }
  }

  renderer.setOrientation(orig_orientation);
}

void LyraTheme::drawSideButtonHints(const GfxRenderer& renderer, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = renderer.getScreenWidth();
  constexpr int buttonWidth = LyraMetrics::values.sideButtonHintsWidth;  // Width on screen (height when rotated)
  constexpr int buttonHeight = 78;                                       // Height on screen (width when rotated)
  // Position for the button group - buttons share a border so they're adjacent

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonWidth;

  // Draw top button outline
  if (topBtn != nullptr && topBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY, buttonWidth, buttonHeight, 1, cornerRadius, true, false, true, false,
                             true);
  }

  // Draw bottom button outline
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    renderer.drawRoundedRect(x, topHintButtonY + buttonHeight + 5, buttonWidth, buttonHeight, 1, cornerRadius, true,
                             false, true, false, true);
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topHintButtonY + (i * buttonHeight + 5);

      // Draw rotated text centered in the button
      const int textWidth = renderer.getTextWidth(SMALL_FONT_ID, labels[i]);

      renderer.drawTextRotated90CW(SMALL_FONT_ID, x, y + (buttonHeight + textWidth) / 2, labels[i]);
    }
  }
}

void LyraTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                    const int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                    bool& bufferRestored, std::function<bool()> storeCoverBuffer) const {
  const int tileWidth = (rect.width - 2 * LyraMetrics::values.contentSidePadding) / 3;
  const int tileHeight = rect.height;
  const int bookTitleHeight = tileHeight - LyraMetrics::values.homeCoverHeight - hPaddingInSelection;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount);
           i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          LyraMetrics::values.homeCoverHeight);
        if (!coverPath.empty()) {
          const std::string coverBmpPath = UITheme::getCoverThumbPath(coverPath, LyraMetrics::values.homeCoverHeight);

          // First time: load cover from SD and render
          FsFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(LyraMetrics::values.homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);
              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection, LyraMetrics::values.homeCoverHeight, cropX);
            }
            file.close();
          }
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = true;
    }

    for (int i = 0; i < std::min(static_cast<int>(recentBooks.size()), LyraMetrics::values.homeRecentBooksCount); i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = LyraMetrics::values.contentSidePadding + tileWidth * i;
      auto title =
          renderer.truncatedText(EDSLAB_10_FONT_ID, recentBooks[i].title.c_str(), tileWidth - 2 * hPaddingInSelection);

      if (bookSelected) {
        // Draw selection box
        renderer.fillRoundedRect(tileX, tileY, tileWidth, hPaddingInSelection, cornerRadius, true, true, false, false,
                                 Color::LightGray);
        renderer.fillRectDither(tileX, tileY + hPaddingInSelection, hPaddingInSelection,
                                LyraMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRectDither(tileX + tileWidth - hPaddingInSelection, tileY + hPaddingInSelection,
                                hPaddingInSelection, LyraMetrics::values.homeCoverHeight, Color::LightGray);
        renderer.fillRoundedRect(tileX, tileY + LyraMetrics::values.homeCoverHeight + hPaddingInSelection, tileWidth,
                                 bookTitleHeight, cornerRadius, false, false, true, true, Color::LightGray);
      }
      renderer.drawText(EDSLAB_10_FONT_ID, tileX + hPaddingInSelection,
                        tileY + tileHeight - bookTitleHeight + hPaddingInSelection + 5, title.c_str(), true);
    }
  }
}

namespace {
void drawMenuIcon(const GfxRenderer& renderer, const std::string& iconName, int x, int y) {
  constexpr int s = 16;  // icon size
  if (iconName == "folder") {
    renderer.drawRect(x, y + 4, s, s - 4);
    renderer.drawRect(x, y + 2, 8, 4);
  } else if (iconName == "grid") {
    constexpr int g = 2;
    constexpr int c = (s - g) / 2;
    renderer.fillRect(x, y, c, c);
    renderer.fillRect(x + c + g, y, c, c);
    renderer.fillRect(x, y + c + g, c, c);
    renderer.fillRect(x + c + g, y + c + g, c, c);
  } else if (iconName == "clock") {
    constexpr int r = s / 2;
    constexpr int cx = r;
    constexpr int cy = r;
    // Circle outline (octagon approximation)
    renderer.drawLine(x + cx - r, y + cy - 2, x + cx - r, y + cy + 2);      // left
    renderer.drawLine(x + cx + r, y + cy - 2, x + cx + r, y + cy + 2);      // right
    renderer.drawLine(x + cx - 2, y + cy - r, x + cx + 2, y + cy - r);      // top
    renderer.drawLine(x + cx - 2, y + cy + r, x + cx + 2, y + cy + r);      // bottom
    renderer.drawLine(x + cx - r, y + cy - 2, x + cx - 2, y + cy - r);      // top-left
    renderer.drawLine(x + cx + 2, y + cy - r, x + cx + r, y + cy - 2);      // top-right
    renderer.drawLine(x + cx - r, y + cy + 2, x + cx - 2, y + cy + r);      // bottom-left
    renderer.drawLine(x + cx + 2, y + cy + r, x + cx + r, y + cy + 2);      // bottom-right
    // Hands
    renderer.drawLine(x + cx, y + cy, x + cx, y + cy - r + 3);  // minute (up)
    renderer.drawLine(x + cx, y + cy, x + cx + r - 4, y + cy);  // hour (right)
  } else if (iconName == "transfer") {
    // Up arrow
    renderer.drawLine(x + 4, y + 2, x + 4, y + 8);
    renderer.drawLine(x + 4, y + 2, x + 2, y + 4);
    renderer.drawLine(x + 4, y + 2, x + 6, y + 4);
    // Down arrow
    renderer.drawLine(x + 11, y + 7, x + 11, y + 13);
    renderer.drawLine(x + 11, y + 13, x + 9, y + 11);
    renderer.drawLine(x + 11, y + 13, x + 13, y + 11);
  } else if (iconName == "gear") {
    constexpr int cx = s / 2;
    constexpr int cy = s / 2;
    renderer.fillRect(x + cx - 2, y + cy - 2, 5, 5);       // center
    renderer.drawLine(x + cx, y, x + cx, y + s);            // vertical
    renderer.drawLine(x, y + cy, x + s, y + cy);            // horizontal
    renderer.drawLine(x + 2, y + 2, x + s - 2, y + s - 2); // diagonal
    renderer.drawLine(x + s - 2, y + 2, x + 2, y + s - 2); // diagonal
  } else if (iconName == "globe") {
    constexpr int r = s / 2;
    constexpr int cx = r;
    constexpr int cy = r;
    // Circle (octagon)
    renderer.drawLine(x + cx - r, y + cy - 2, x + cx - r, y + cy + 2);
    renderer.drawLine(x + cx + r, y + cy - 2, x + cx + r, y + cy + 2);
    renderer.drawLine(x + cx - 2, y + cy - r, x + cx + 2, y + cy - r);
    renderer.drawLine(x + cx - 2, y + cy + r, x + cx + 2, y + cy + r);
    renderer.drawLine(x + cx - r, y + cy - 2, x + cx - 2, y + cy - r);
    renderer.drawLine(x + cx + 2, y + cy - r, x + cx + r, y + cy - 2);
    renderer.drawLine(x + cx - r, y + cy + 2, x + cx - 2, y + cy + r);
    renderer.drawLine(x + cx + 2, y + cy + r, x + cx + r, y + cy + 2);
    // Horizontal line
    renderer.drawLine(x, y + cy, x + s, y + cy);
    // Vertical ellipse
    renderer.drawLine(x + cx, y, x + cx, y + s);
  }
}
}  // namespace

void LyraTheme::drawButtonMenu(GfxRenderer& renderer, Rect rect, int buttonCount, int selectedIndex,
                               const std::function<std::string(int index)>& buttonLabel,
                               const std::function<std::string(int index)>& rowIcon) const {
  for (int i = 0; i < buttonCount; ++i) {
    int tileWidth = (rect.width - LyraMetrics::values.contentSidePadding * 2 - LyraMetrics::values.menuSpacing) / 2;
    Rect tileRect =
        Rect{rect.x + LyraMetrics::values.contentSidePadding + (LyraMetrics::values.menuSpacing + tileWidth) * (i % 2),
             rect.y + static_cast<int>(i / 2) * (LyraMetrics::values.menuRowHeight + LyraMetrics::values.menuSpacing),
             tileWidth, LyraMetrics::values.menuRowHeight};

    const bool selected = selectedIndex == i;

    if (selected) {
      renderer.fillRoundedRect(tileRect.x, tileRect.y, tileRect.width, tileRect.height, cornerRadius, Color::LightGray);
    }

    std::string labelStr = buttonLabel(i);
    const char* label = labelStr.c_str();

    const int textWidth = renderer.getTextWidth(EDSLAB_14_FONT_ID, label);
    const int lineHeight = renderer.getLineHeight(EDSLAB_14_FONT_ID);
    const int textY = tileRect.y + (LyraMetrics::values.menuRowHeight - lineHeight) / 2;
    const int textX = tileRect.x + (tileRect.width - textWidth) / 2;
    renderer.drawText(EDSLAB_14_FONT_ID, textX, textY, label, true);
  }
}

void LyraTheme::drawCoverGrid(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex, int pageOffset,
                              const std::function<std::string(int)>& getTitle,
                              const std::function<std::string(int)>& getThumbPath,
                              const std::function<bool(int)>& isDirectory) const {
  constexpr int COLS = 3;
  constexpr int ROWS = 3;
  constexpr int CELL_PADDING = 6;
  constexpr int TITLE_AREA = 24;  // small font height + gap below cover

  const int cellWidth = rect.width / COLS;
  const int cellHeight = rect.height / ROWS;

  // Fill the cell with the cover (no fixed aspect ratio)
  int thumbWidth = cellWidth - CELL_PADDING * 2;
  int thumbHeight = cellHeight - CELL_PADDING * 2 - TITLE_AREA;

  const int pageEnd = std::min(pageOffset + COLS * ROWS, itemCount);

  for (int i = pageOffset; i < pageEnd; i++) {
    const int gridIdx = i - pageOffset;
    const int col = gridIdx % COLS;
    const int row = gridIdx / COLS;

    const int cellX = rect.x + col * cellWidth;
    const int cellY = rect.y + row * cellHeight;

    const bool selected = (i == selectedIndex);

    // Draw selection with rounded rect (Lyra style)
    if (selected) {
      renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, cornerRadius, Color::LightGray);
    }

    // Center the thumbnail area in the cell
    const int thumbX = cellX + (cellWidth - thumbWidth) / 2;
    const int thumbY = cellY + CELL_PADDING;

    std::string thumbPath = getThumbPath(i);
    bool drewCover = false;

    if (!thumbPath.empty() && !isDirectory(i)) {
      FsFile file;
      if (Storage.openFileForRead("LIB", thumbPath, file)) {
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
            }
            renderer.drawBitmap(bitmap, coverX, coverY, thumbWidth, thumbHeight);
            drewCover = true;
          }
        }
        file.close();
      }
    }

    if (!drewCover) {
      if (isDirectory(i)) {
        const int folderW = 80, bodyH = 50, tabW = 28, tabH = 12;
        const int folderX = thumbX + (thumbWidth - folderW) / 2;
        const int folderY = thumbY + (thumbHeight - (bodyH + tabH - 2)) / 2;
        renderer.drawRoundedRect(folderX, folderY, tabW, tabH, 2, 4, true, true, false, false, true);
        renderer.drawRoundedRect(folderX, folderY + tabH - 2, folderW, bodyH, 2, 6, true);
      }
    }

    // Draw title below thumbnail
    std::string title = getTitle(i);
    const int titleY = thumbY + thumbHeight + 1;
    const int maxTitleWidth = cellWidth - CELL_PADDING * 2;
    auto truncated = renderer.truncatedText(EDSLAB_10_FONT_ID, title.c_str(), maxTitleWidth);
    const int titleTextWidth = renderer.getTextWidth(EDSLAB_10_FONT_ID, truncated.c_str());
    const int titleX = cellX + (cellWidth - titleTextWidth) / 2;
    renderer.drawText(EDSLAB_10_FONT_ID, titleX, titleY, truncated.c_str(), true);
  }
}

void LyraTheme::drawCoverGridSelection(GfxRenderer& renderer, Rect rect, int itemCount, int selectedIndex,
                                       int pageOffset, const std::function<std::string(int)>& getTitle,
                                       const std::function<std::string(int)>& getThumbPath,
                                       const std::function<bool(int)>& isDirectory) const {
  if (selectedIndex < pageOffset || selectedIndex >= std::min(pageOffset + 9, itemCount)) return;

  constexpr int COLS = 3;
  constexpr int CELL_PADDING = 6;
  constexpr int TITLE_AREA = 24;

  const int cellWidth = rect.width / COLS;
  const int cellHeight = rect.height / 3;
  const int thumbWidth = cellWidth - CELL_PADDING * 2;
  const int thumbHeight = cellHeight - CELL_PADDING * 2 - TITLE_AREA;

  const int gridIdx = selectedIndex - pageOffset;
  const int col = gridIdx % COLS;
  const int row = gridIdx / COLS;

  const int cellX = rect.x + col * cellWidth;
  const int cellY = rect.y + row * cellHeight;

  // Draw selection fill (Lyra style)
  renderer.fillRoundedRect(cellX + 2, cellY + 2, cellWidth - 4, cellHeight - 4, cornerRadius, Color::LightGray);

  const int thumbX = cellX + (cellWidth - thumbWidth) / 2;
  const int thumbY = cellY + CELL_PADDING;

  // Re-read and draw the single selected cover
  std::string thumbPath = getThumbPath(selectedIndex);
  bool drewCover = false;

  if (!thumbPath.empty() && !isDirectory(selectedIndex)) {
    FsFile file;
    if (Storage.openFileForRead("LIB", thumbPath, file)) {
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
          }
          renderer.drawBitmap(bitmap, coverX, coverY, thumbWidth, thumbHeight);
          drewCover = true;
        }
      }
      file.close();
    }
  }

  if (!drewCover) {
    if (isDirectory(selectedIndex)) {
      const int folderW = 80, bodyH = 50, tabW = 28, tabH = 12;
      const int folderX = thumbX + (thumbWidth - folderW) / 2;
      const int folderY = thumbY + (thumbHeight - (bodyH + tabH - 2)) / 2;
      renderer.drawRoundedRect(folderX, folderY, tabW, tabH, 2, 4, true, true, false, false, true);
      renderer.drawRoundedRect(folderX, folderY + tabH - 2, folderW, bodyH, 2, 6, true);
    }
  }

  // Draw title
  std::string title = getTitle(selectedIndex);
  const int titleY = thumbY + thumbHeight + 1;
  const int maxTitleWidth = cellWidth - CELL_PADDING * 2;
  auto truncated = renderer.truncatedText(EDSLAB_10_FONT_ID, title.c_str(), maxTitleWidth);
  const int titleTextWidth = renderer.getTextWidth(EDSLAB_10_FONT_ID, truncated.c_str());
  const int titleX = cellX + (cellWidth - titleTextWidth) / 2;
  renderer.drawText(EDSLAB_10_FONT_ID, titleX, titleY, truncated.c_str(), true);
}

Rect LyraTheme::drawPopup(const GfxRenderer& renderer, const char* message) const {
  constexpr int margin = 15;
  constexpr int y = 60;
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::REGULAR);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + margin * 2;
  const int h = textHeight + margin * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;

  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, false);
  renderer.drawRect(x, y, w, h, true);

  const int textX = x + (w - textWidth) / 2;
  const int textY = y + margin - 2;
  renderer.drawText(UI_12_FONT_ID, textX, textY, message, true, EpdFontFamily::REGULAR);
  renderer.displayBuffer();
  return Rect{x, y, w, h};
}
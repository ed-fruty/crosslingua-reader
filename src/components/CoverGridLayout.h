#pragma once

// Single source of truth for the BookShelf cover-grid geometry. BookShelfActivity derives
// thumbHeight from these (which becomes the Epub/Xtc generateThumbBmp size AND the thumb cache
// filename `thumb_<h>.bmp`), while the themes derive the cell/letterbox math from the same
// numbers. Diverging copies would silently generate thumbs at a size the display box no longer
// matches — keep every consumer on this header.
namespace covergrid {
constexpr int GRID_COLS = 3;
constexpr int GRID_ROWS = 3;
constexpr int GRID_PAGE_ITEMS = GRID_COLS * GRID_ROWS;
constexpr int GRID_CELL_PADDING = 6;
constexpr int GRID_TITLE_AREA = 24;  // small-font height + gap below cover
}  // namespace covergrid

#include "components/CrossLinguaCoverLogo.h"

#include <algorithm>
#include <cstdint>

#include "components/icons/crosslinguaLogo.h"

namespace crosslingua {
namespace {

struct LogoSize {
  const uint8_t* bitmap;
  int size;
};

// Largest first. The logo fills at most ~62% of the area's shorter side.
constexpr LogoSize LOGOS[] = {{CrossLinguaLogo96, 96}, {CrossLinguaLogo64, 64}, {CrossLinguaLogo40, 40}};

}  // namespace

void drawCoverPlaceholderLogo(const GfxRenderer& renderer, const Rect& area) {
  const int limit = std::min(area.width, area.height) * 62 / 100;
  for (const LogoSize& logo : LOGOS) {
    if (logo.size > limit) continue;
    const int x = area.x + (area.width - logo.size) / 2;
    const int y = area.y + (area.height - logo.size) / 2;
    // Same bit layout and orientation mapping as GfxRenderer::drawIcon, but the ink is drawn white
    // because the placeholder's lower part is black.
    const int rowBytes = (logo.size + 7) / 8;
    for (int row = 0; row < logo.size; row++) {
      for (int col = 0; col < logo.size; col++) {
        const uint8_t byte = logo.bitmap[row * rowBytes + (col >> 3)];
        if (((byte >> (7 - (col & 7))) & 1) == 0) renderer.drawPixel(x + (logo.size - 1 - row), y + col, false);
      }
    }
    return;
  }
}

}  // namespace crosslingua

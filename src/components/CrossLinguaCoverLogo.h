#pragma once

#include <GfxRenderer.h>

#include "components/themes/BaseTheme.h"

namespace crosslingua {

// Draws the white "CL" logo centred in `area`, which the caller has already filled black (the lower
// part of the no-cover placeholder). Picks the largest pre-rendered size that fits; draws nothing
// when even the smallest does not.
void drawCoverPlaceholderLogo(const GfxRenderer& renderer, const Rect& area);

}  // namespace crosslingua

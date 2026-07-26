#pragma once
#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"
#include "I18nKeys.h"

// Single source of truth for the USER-SELECTABLE Pre-Translation display modes and their labels.
//
// CrossPointSettings::PRE_TRANSLATION_MODE is the persisted value space and contains two permanent
// holes (1 / 2, the retired Dimmed modes). It is therefore NOT iterable: `% PT_MODE_COUNT` would
// walk straight into the holes. Everything user-facing — the Lingua submenu cycle, the
// post-translation chooser lists — walks PT_SELECTABLE_MODES instead, in the order defined here,
// which is the order the user sees.

// UI order. Appending a mode here (and to PRE_TRANSLATION_MODE) is all a new mode needs to appear
// in every list and cycle.
//
// PT_INTERLINEAR is DELIBERATELY ABSENT. The enum value is reserved and its label is translated,
// but the layout does not exist yet (CrossPointSettings::ptLayoutForDisplayMode maps it to
// PtLayout::Both), so offering it would let the user select and persist a mode that renders
// exactly like Normal. TO RE-ADD once the Interlinear layout lands: append
// `CrossPointSettings::PT_INTERLINEAR` to the table below (last, after PT_PAGE_TRANSLATION — appending is
// what keeps the cycle order stable for users), and change the static_assert's `- 3` back to
// `- 2` together with its message.
inline constexpr CrossPointSettings::PRE_TRANSLATION_MODE PT_SELECTABLE_MODES[] = {
    CrossPointSettings::PT_NORMAL,           CrossPointSettings::PT_INTERLEAVED,
    CrossPointSettings::PT_SIDE_BY_SIDE,     CrossPointSettings::PT_ORIGINAL_ONLY,
    CrossPointSettings::PT_TRANSLATION_ONLY, CrossPointSettings::PT_TOOLTIP,
    CrossPointSettings::PT_PAGE_TRANSLATION,
};

inline constexpr size_t PT_SELECTABLE_MODE_COUNT = sizeof(PT_SELECTABLE_MODES) / sizeof(PT_SELECTABLE_MODES[0]);

// Every persisted value is either selectable, one of the two retired holes, or PT_INTERLINEAR
// (reserved, not yet implemented — see above). A new mode that is added to the enum but forgotten
// here (or vice versa) fails the build.
static_assert(PT_SELECTABLE_MODE_COUNT == static_cast<size_t>(CrossPointSettings::PT_MODE_COUNT) - 3,
              "PT_SELECTABLE_MODES must list every PRE_TRANSLATION_MODE except the two retired holes "
              "(1, 2) and the not-yet-implemented PT_INTERLINEAR");

// The ONE mode -> label mapping (it replaced three duplicated DISPLAY_MODE_LABELS tables).
// Deliberately a switch with NO `default:` case: a mode added to PRE_TRANSLATION_MODE without a
// label here is then a -Wswitch diagnostic at compile time instead of a wrong label at runtime.
constexpr StrId ptModeLabel(const CrossPointSettings::PRE_TRANSLATION_MODE mode) {
  switch (mode) {
    case CrossPointSettings::PT_NORMAL:
      return StrId::STR_PT_NORMAL;
    case CrossPointSettings::PT_INTERLEAVED:
      return StrId::STR_PT_INTERLEAVED;
    case CrossPointSettings::PT_SIDE_BY_SIDE:
      return StrId::STR_PT_SIDE_BY_SIDE;
    case CrossPointSettings::PT_ORIGINAL_ONLY:
      return StrId::STR_PT_ORIGINAL_ONLY;
    case CrossPointSettings::PT_TRANSLATION_ONLY:
      return StrId::STR_PT_TRANSLATION_ONLY;
    case CrossPointSettings::PT_TOOLTIP:
      return StrId::STR_PT_TOOLTIP;
    case CrossPointSettings::PT_PAGE_TRANSLATION:
      return StrId::STR_PT_PAGE_TRANSLATION;
    case CrossPointSettings::PT_INTERLINEAR:
      return StrId::STR_PT_INTERLINEAR;
    // Retired holes. Unreachable through the UI (never selectable) and unreachable from settings
    // (fromJson migrates them away), but labelled so the switch stays exhaustive without a
    // `default:`. The strings are the ones those modes used to show.
    case CrossPointSettings::PT_LEGACY_DIMMED:
      return StrId::STR_PT_DARK;
    case CrossPointSettings::PT_LEGACY_DIMMED_LIGHT:
      return StrId::STR_PT_LIGHT;
  }
  return StrId::STR_PT_NORMAL;  // unreachable: every enumerator returns above
}

// Position of a persisted mode value in the selectable list, for seeding a cursor. A retired hole
// or an out-of-range value maps to 0 (Normal) rather than off the end.
constexpr size_t ptSelectableIndex(const uint8_t mode) {
  for (size_t i = 0; i < PT_SELECTABLE_MODE_COUNT; i++) {
    if (static_cast<uint8_t>(PT_SELECTABLE_MODES[i]) == mode) return i;
  }
  return 0;
}

// Next mode in UI order, wrapping. Walks the selectable table, so the retired holes can never be
// reached by cycling.
constexpr uint8_t ptNextSelectableMode(const uint8_t mode) {
  return static_cast<uint8_t>(PT_SELECTABLE_MODES[(ptSelectableIndex(mode) + 1) % PT_SELECTABLE_MODE_COUNT]);
}

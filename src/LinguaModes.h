#pragma once
#include <cstddef>
#include <cstdint>

#include "CrossPointSettings.h"
#include "I18nKeys.h"

// Single source of truth for the USER-SELECTABLE Lingua display modes and their labels.
//
// CrossPointSettings::LINGUA_MODE is the persisted value space and contains two permanent
// holes (1 / 2, the retired Dimmed modes). It is therefore NOT iterable: `% LINGUA_MODE_COUNT` would
// walk straight into the holes. Everything user-facing — the Lingua submenu cycle, the
// post-translation chooser lists — walks LINGUA_SELECTABLE_MODES instead, in the order defined here,
// which is the order the user sees.

// UI order. Appending a mode here (and to LINGUA_MODE) is all a new mode needs to appear
// in every list and cycle.
//
// LINGUA_INTERLINEAR is LAST deliberately: it was added once its layout landed (LinguaLayout::Interlinear),
// and appending rather than inserting is what keeps every existing user's cycle order unchanged.
inline constexpr CrossPointSettings::LINGUA_MODE LINGUA_SELECTABLE_MODES[] = {
    CrossPointSettings::LINGUA_NORMAL,           CrossPointSettings::LINGUA_INTERLEAVED,
    CrossPointSettings::LINGUA_SIDE_BY_SIDE,     CrossPointSettings::LINGUA_ORIGINAL_ONLY,
    CrossPointSettings::LINGUA_TRANSLATION_ONLY, CrossPointSettings::LINGUA_TOOLTIP,
    CrossPointSettings::LINGUA_PAGE_TRANSLATION, CrossPointSettings::LINGUA_INTERLINEAR,
};

inline constexpr size_t LINGUA_SELECTABLE_MODE_COUNT = sizeof(LINGUA_SELECTABLE_MODES) / sizeof(LINGUA_SELECTABLE_MODES[0]);

// Every persisted value is either selectable or one of the two retired holes. A new mode that is
// added to the enum but forgotten here (or vice versa) fails the build.
static_assert(LINGUA_SELECTABLE_MODE_COUNT == static_cast<size_t>(CrossPointSettings::LINGUA_MODE_COUNT) - 2,
              "LINGUA_SELECTABLE_MODES must list every LINGUA_MODE except the two retired holes (1, 2)");

// The ONE mode -> label mapping (it replaced three duplicated DISPLAY_MODE_LABELS tables).
// Deliberately a switch with NO `default:` case: a mode added to LINGUA_MODE without a
// label here is then a -Wswitch diagnostic at compile time instead of a wrong label at runtime.
constexpr StrId linguaModeLabel(const CrossPointSettings::LINGUA_MODE mode) {
  switch (mode) {
    case CrossPointSettings::LINGUA_NORMAL:
      return StrId::STR_PT_NORMAL;
    case CrossPointSettings::LINGUA_INTERLEAVED:
      return StrId::STR_PT_INTERLEAVED;
    case CrossPointSettings::LINGUA_SIDE_BY_SIDE:
      return StrId::STR_PT_SIDE_BY_SIDE;
    case CrossPointSettings::LINGUA_ORIGINAL_ONLY:
      return StrId::STR_PT_ORIGINAL_ONLY;
    case CrossPointSettings::LINGUA_TRANSLATION_ONLY:
      return StrId::STR_PT_TRANSLATION_ONLY;
    case CrossPointSettings::LINGUA_TOOLTIP:
      return StrId::STR_PT_TOOLTIP;
    case CrossPointSettings::LINGUA_PAGE_TRANSLATION:
      return StrId::STR_PT_PAGE_TRANSLATION;
    case CrossPointSettings::LINGUA_INTERLINEAR:
      return StrId::STR_PT_INTERLINEAR;
    // Retired holes. Unreachable through the UI (never selectable) and unreachable from settings
    // (fromJson migrates them away), but labelled so the switch stays exhaustive without a
    // `default:`. The strings are the ones those modes used to show.
    case CrossPointSettings::LINGUA_LEGACY_DIMMED:
      return StrId::STR_PT_DARK;
    case CrossPointSettings::LINGUA_LEGACY_DIMMED_LIGHT:
      return StrId::STR_PT_LIGHT;
  }
  return StrId::STR_PT_NORMAL;  // unreachable: every enumerator returns above
}

// Position of a persisted mode value in the selectable list, for seeding a cursor. A retired hole
// or an out-of-range value maps to 0 (Normal) rather than off the end.
constexpr size_t linguaSelectableIndex(const uint8_t mode) {
  for (size_t i = 0; i < LINGUA_SELECTABLE_MODE_COUNT; i++) {
    if (static_cast<uint8_t>(LINGUA_SELECTABLE_MODES[i]) == mode) return i;
  }
  return 0;
}

// Next mode in UI order, wrapping. Walks the selectable table, so the retired holes can never be
// reached by cycling.
constexpr uint8_t linguaNextSelectableMode(const uint8_t mode) {
  return static_cast<uint8_t>(LINGUA_SELECTABLE_MODES[(linguaSelectableIndex(mode) + 1) % LINGUA_SELECTABLE_MODE_COUNT]);
}

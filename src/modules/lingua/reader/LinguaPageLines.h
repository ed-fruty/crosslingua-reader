#pragma once

// Lingua hooks for upstream code that walks a page's lines word by word (clip selection, saved
// clipping hit-tests and highlights). Free functions rather than PageLine members so upstream's
// standalone UI regressions, which compile those walks against their own PageLine stub, only need
// matching stubs of these two names.
namespace lingua {

// Interlinear / Word Lens annotation rows are reader-inserted text, not the chapter's own words.
template <class Line>
inline bool isAnnotationLine(const Line& line) {
  return line.isLinguaAnnotation();
}

// The face a line was laid out and drawn in; a page can mix body, translation and annotation sizes.
template <class Fonts, class Line>
inline int lineFontId(const Fonts& fonts, const Line& line) {
  return fonts.forRole(line.fontRole);
}

}  // namespace lingua

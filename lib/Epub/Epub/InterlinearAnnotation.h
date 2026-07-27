#pragma once
#include <cstdint>

// PtLayout::Interlinear: the boundary between the LAYOUT (this library) and the sentence ALIGNMENT
// (the app).
//
// lib/Epub owns everything geometric: how a sentence start constrains line breaking, how tall an
// 8pt annotation row is, and when a page must break so an annotation never leaves the line it
// belongs to. It deliberately owns NONE of "which translated sentence belongs above which source
// sentence" — that heuristic is shared with the Tooltip display mode, is host-unit-tested
// (test/test_sentence_splitter) and lives in src/translator/SentencePairing. Injecting it as a
// plain function pointer is what keeps this library free of an app-level include, exactly as
// PtLayout keeps it free of the app's display-mode enum.

// One annotation: the translated words belonging to one source sentence group, which
// renderInterlinear then flows across the annotation strips sitting above that sentence's source
// lines — one line of translation per line of source. A "group" is the run of consecutive source
// sentences that resolve to the same translation (an engine that merged K source sentences into one
// produces such a run); it gets ONE annotation, so the identical text is never printed K times.
struct InterlinearAnnotation {
  // Index into the source word array as the caller passed it, i.e. PRE-layout and in logical order.
  // renderInterlinear hands these to layout as TRACKED WORDS (ParsedText::setTrackedWords), which
  // CONSTRAIN NOTHING — the source breaks exactly as an untranslated paragraph would. Layout reports
  // back the line and the x each one landed on, and that x is where the sentence's translation
  // starts. They must be pre-layout because the pairing runs before line breaking, not after it.
  uint16_t sourceStartWord;
  uint16_t transStartWord;  // [transStartWord, transEndWord) into the translation word array
  uint16_t transEndWord;    // == transStartWord means "no translation": emit no row
};

// Upper bound on annotations per paragraph, and therefore the size of the parser's one reusable
// annotation buffer (300 bytes). A paragraph cannot yield more annotations than it has sentences,
// and the splitter itself caps a paragraph at 50 sentences, so this is never the binding limit.
constexpr int INTERLINEAR_MAX_ANNOTATIONS = 50;

// Supplied by the app (src/translator/InterlinearPairing.h: interlinearPairSentences). Reads both
// word arrays, writes at most maxOut annotations in ascending sourceStartWord order, and returns how
// many. Returns 0 for "nothing could be aligned", which the layout path treats as "emit the source
// paragraph with no annotation rows". Both word arrays are NUL-terminated C strings owned by the
// caller; the callee must not retain them.
using InterlinearPairFn = int (*)(const char* const* sourceWords, int sourceWordCount,
                                  const char* const* translationWords, int translationWordCount,
                                  InterlinearAnnotation* out, int maxOut);

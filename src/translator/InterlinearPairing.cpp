#include "InterlinearPairing.h"

#include <Logging.h>
#include <Memory.h>

#include "SentencePairing.h"

int interlinearPairSentences(const char* const* sourceWords, const int sourceWordCount,
                             const char* const* translationWords, const int translationWordCount,
                             InterlinearAnnotation* out, const int maxOut) {
  if (!sourceWords || !translationWords || !out || maxOut <= 0) return 0;

  // The aligner's fixed scratch is ~800 bytes — far past the <256 B stack-local budget, and the
  // layout path runs inside the expat callback chain where the stack is already deep. It cannot be
  // owned by the parser instead (the parser is in lib/Epub and must not name an app type), so it is
  // allocated per paragraph pair. That is a uniform-size alloc immediately followed by its free, so
  // the allocator hands back the same block every time: no net fragmentation, and nothing resident
  // for the users who never select Interlinear.
  const auto scratch = makeUniqueNoThrow<SentencePairScratch>();
  if (!scratch) {
    LOG_ERR("ILN", "OOM: sentence pairing scratch");
    return 0;
  }

  if (!splitSentencePair(sourceWords, sourceWordCount, translationWords, translationWordCount, *scratch)) return 0;
  // Same junk merge the tooltip applies to a page's sentences, for the same reason: a stray "." left
  // by a spaced ellipsis must not claim an annotation row of its own.
  mergeJunkSentences(scratch->origSplits, sourceWords);
  mapSentenceSpans(sourceWords, translationWords, *scratch);

  // 200 B, inside the stack budget. Steps <= sentences <= MAX_SENTENCES by construction.
  SentenceStep steps[MAX_SENTENCES];
  const int stepCount = groupTranslationSpanSteps(scratch->transFor, scratch->origSplits.count, steps, MAX_SENTENCES);

  int written = 0;
  for (int s = 0; s < stepCount && written < maxOut; s++) {
    const int first = steps[s].firstSentence;
    // Anchored at the group's FIRST sentence: the run shares one translation, so it gets one row-set
    // sitting above where the run starts.
    out[written].sourceStartWord = scratch->origSplits.spans[first].startWord;
    out[written].transStartWord = scratch->transFor[first].startWord;
    out[written].transEndWord = scratch->transFor[first].endWord;
    written++;
  }
  return written;
}

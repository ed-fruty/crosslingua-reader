#pragma once
#include <Print.h>
#include <expat.h>

#include <cstdint>
#include <string>
#include <vector>

/**
 * SAX-based HTML rewriter that inserts machine-translated paragraphs after
 * each block element in an EPUB chapter.
 *
 * Strategy: reconstruct the original markup from expat callbacks, and after
 * each closing block tag (p, h1-h6, li, blockquote) append a translated
 * paragraph marked with a lang attribute (<p lang="xx">).
 */
class TranslatingHtmlRewriter {
 public:
  struct Result {
    int paragraphsTranslated = 0;
    int paragraphsSkipped = 0;
    // Paragraphs whose translation genuinely failed after retries were exhausted (or the
    // chapter was aborted mid-translation). Distinct from paragraphsSkipped, which also
    // counts already-translated/empty-source blocks that were never sent to the engine.
    int translateFailures = 0;
    bool cancelled = false;
    bool abortedOnErrors = false;
    char errorDetail[64] = {};  // last error message when abortedOnErrors
  };

  // Count translatable block elements in a file without translating.
  // Used for progress bar total.
  static int countBlocksInFile(const std::string& inputPath);

  // Check if a chapter HTML file contains embedded translations (Calibre or CrossPoint).
  // Lightweight SAX scan — stops at the first block element with a `lang` attribute.
  static bool hasEmbeddedTranslations(const std::string& inputPath);

  // Rewrite HTML from `inputBuf` (size `inputSize`) into `out`.
  // Returns summary of what happened.
  Result rewrite(const char* inputBuf, size_t inputSize, Print& out, const char* sourceLang, const char* targetLang,
                 uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                 volatile int* progressOut = nullptr);

  // Invoked at every batch boundary (after a batch's translations are written and
  // progressOut is updated, with the batch's TLS/HTTP transients already torn down).
  // Runs on the calling task. Lets a caller repaint progress at a point where the heap
  // has a clean hole. C-style pointer + context to avoid std::function heap/bloat.
  using BatchBoundaryCb = void (*)(void* ctx);

  // Rewrite HTML from a file on SD card into `out`, reading in 1KB chunks.
  // This avoids loading the entire chapter HTML into memory.
  // Optional boundaryCb fires between batches (see BatchBoundaryCb); pass nullptr to
  // opt out (whole-file/book callers that repaint only at their own boundaries).
  Result rewriteFromFile(const std::string& inputPath, Print& out, const char* sourceLang, const char* targetLang,
                         uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                         volatile int* progressOut = nullptr, BatchBoundaryCb boundaryCb = nullptr,
                         void* boundaryCtx = nullptr);

 private:
  Print* out = nullptr;
  const char* sourceLang = nullptr;
  const char* targetLang = nullptr;
  uint8_t engine = 0;
  const char* apiKey = nullptr;
  volatile const bool* cancelled = nullptr;
  volatile int* progressOut = nullptr;
  BatchBoundaryCb onBatchBoundary = nullptr;  // between-batch repaint hook; null = disabled
  void* batchBoundaryCtx = nullptr;

  int depth = 0;
  int blockDepth = -1;  // depth where current block element began; -1 = not in block
  bool insideHead = false;
  bool wroteXmlDecl = false;

  std::string blockHtml;     // Reconstructed markup of current block (for output)
  std::string blockText;     // Plain text of current block (for translation)
  std::string blockTagName;  // tag name of current block (e.g., "p", "h1")
  std::string blockClass;    // class attribute of current block

  int skipBlockDepth = -1;  // depth of a block being skipped (existing translation)

  int paragraphsTranslated = 0;
  int paragraphsSkipped = 0;
  int translateFailures = 0;  // genuine translate failures (see Result::translateFailures)
  int blocksProcessed = 0;    // all batch entries including empty blocks (for progress bar)
  bool wasCancelled = false;
  int consecutiveFailures = 0;   // reset on success, increment on failure
  bool abortedOnErrors = false;  // set when consecutiveFailures hits threshold
  std::string lastError;         // last translation error message
  static constexpr int MAX_CONSECUTIVE_FAILURES = 20;

  // ─── Network retry/backoff (see HttpDownloader::lastHttpCode) ───────────────
  int consecutive429 = 0;  // consecutive HTTP 429 responses; reset on any non-429 outcome
  static constexpr int MAX_CONSECUTIVE_429 = 3;

  // Classify a failed translate attempt (httpCode = HttpDownloader::lastHttpCode captured
  // right after the failing call). Updates consecutive429 and, on a non-retryable outcome,
  // sets abortedOnErrors (auth failure or too many consecutive 429s).
  // Returns true if the attempt loop should retry.
  bool shouldRetryAfterFailure(int httpCode);

  // Pure backoff delay (ms) for a given HTTP status code and 0-based retry attempt index.
  // 429 gets a fixed ~1.5s spacing; other transient errors ramp {500, 1500, 3000} ms.
  static int backoffDelayMs(int httpCode, int attempt);

  // ─── Batch buffering ─────────────────────────────────────────────────────
  struct BatchEntry {
    std::string htmlBefore;    // all HTML output accumulated before this entry's translation slot
    std::string trimmedText;   // plain text to translate (empty = untranslatable, skip)
    std::string blockTagName;  // tag name of original block (e.g., "p", "h1")
    std::string blockClass;    // class attribute of original block (for CSS spacing)
  };
  std::vector<BatchEntry> batch;
  std::string pendingHtml;                            // accumulates writeOut() calls between block flushes
  size_t batchTextBytes = 0;                          // running total of trimmedText bytes in current batch
  static constexpr size_t BATCH_TARGET_BYTES = 1500;  // flush threshold (under MAX_TEXT_BYTES=1800)

  static const char* BLOCK_TAGS[];
  static const int NUM_BLOCK_TAGS;

  static bool isBlockTag(const char* name);

  // Append XML-escaped version of `s` to `buf`
  static void appendEscaped(const char* s, size_t len, std::string& buf);

  // Write to pendingHtml buffer (not directly to out)
  void writeOut(const char* s, size_t len);
  void writeOut(const std::string& s);

  // Write directly to output Print stream (bypasses buffer)
  void writeRaw(const char* s, size_t len);
  void writeRaw(const std::string& s);

  // Build opening tag string "<name attr1=...>"
  static std::string makeOpenTag(const XML_Char* name, const XML_Char** atts);

  // Called when a block element closes: accumulate into batch
  void flushBlock(const char* endTagName);

  // Translate all accumulated batch entries in one API call, write to out
  void flushBatch();

  // Split string by "\n\n" separator
  static std::vector<std::string> splitByDoubleLF(const std::string& s);

  static void XMLCALL onStart(void* ud, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEnd(void* ud, const XML_Char* name);
  static void XMLCALL onChars(void* ud, const XML_Char* s, int len);
  static void XMLCALL onDefault(void* ud, const XML_Char* s, int len);

  // Counting-only callbacks (for countBlocksInFile)
  static void XMLCALL onStartCount(void* ud, const XML_Char* name, const XML_Char** atts);

  // Detection callback (for hasEmbeddedTranslations)
  struct EmbeddedDetectState {
    bool found = false;
  };
  static void XMLCALL onStartDetectEmbedded(void* ud, const XML_Char* name, const XML_Char** atts);
};

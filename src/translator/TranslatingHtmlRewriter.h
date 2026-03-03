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
 * paragraph in a grey colour (<p style="color:#5A5A5A">).
 */
class TranslatingHtmlRewriter {
 public:
  struct Result {
    int paragraphsTranslated = 0;
    int paragraphsSkipped = 0;
    bool cancelled = false;
  };

  // Count translatable block elements in a file without translating.
  // Used for progress bar total.
  static int countBlocksInFile(const std::string& inputPath);

  // Rewrite HTML from `inputBuf` (size `inputSize`) into `out`.
  // Returns summary of what happened.
  Result rewrite(const char* inputBuf, size_t inputSize, Print& out, const char* sourceLang, const char* targetLang,
                 uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                 volatile int* progressOut = nullptr);

  // Rewrite HTML from a file on SD card into `out`, reading in 1KB chunks.
  // This avoids loading the entire chapter HTML into memory.
  Result rewriteFromFile(const std::string& inputPath, Print& out, const char* sourceLang, const char* targetLang,
                         uint8_t engine, const char* apiKey, volatile const bool* cancelled,
                         volatile int* progressOut = nullptr);

 private:
  Print* out = nullptr;
  const char* sourceLang = nullptr;
  const char* targetLang = nullptr;
  uint8_t engine = 0;
  const char* apiKey = nullptr;
  volatile const bool* cancelled = nullptr;
  volatile int* progressOut = nullptr;

  int depth = 0;
  int blockDepth = -1;   // depth where current block element began; -1 = not in block
  bool insideHead = false;
  bool wroteXmlDecl = false;

  std::string blockHtml;  // Reconstructed markup of current block (for output)
  std::string blockText;  // Plain text of current block (for translation)

  int paragraphsTranslated = 0;
  int paragraphsSkipped = 0;
  bool wasCancelled = false;

  // ─── Batch buffering ─────────────────────────────────────────────────────
  struct BatchEntry {
    std::string htmlBefore;   // all HTML output accumulated before this entry's translation slot
    std::string trimmedText;  // plain text to translate (empty = untranslatable, skip)
  };
  std::vector<BatchEntry> batch;
  std::string pendingHtml;       // accumulates writeOut() calls between block flushes
  size_t batchTextBytes = 0;     // running total of trimmedText bytes in current batch
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
};

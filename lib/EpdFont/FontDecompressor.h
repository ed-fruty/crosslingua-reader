#pragma once

#include <InflateReader.h>

#include "EpdFontData.h"

class FontDecompressor {
 public:
  static constexpr uint16_t MAX_PAGE_GLYPHS = 512;
  static constexpr uint8_t MAX_PAGE_SLOTS = 4;  // One per font style (R/B/I/BI)

  // Bounded fallback group cache (see the GroupSlot block below). Two slots so the pinned Cyrillic
  // group and the small punctuation group stay warm together; total decompressed bytes are capped so
  // the ceiling matches the old single-group cache rather than growing with slot count.
  // 3 slots: Ukrainian/Russian translated text alternates between THREE groups — Cyrillic
  // letters (group 5), ASCII punctuation/digits (group 0) and smart punctuation «»…— (group 7,
  // every dialogue line) — so two slots would still LRU-thrash on guillemet-heavy text. All
  // three stay within GROUP_CACHE_BUDGET_BYTES even at the largest built-in size.
  static constexpr uint8_t MAX_GROUP_SLOTS = 3;
  static constexpr uint32_t GROUP_CACHE_BUDGET_BYTES = 57344;  // 56 KB cap on resident group buffers

  FontDecompressor() = default;
  ~FontDecompressor();

  bool init();
  void deinit();

  // Returns pointer to decompressed bitmap data for the given glyph.
  // Checks the page buffer (from prewarm) first, then falls back to the hot group slot.
  const uint8_t* getBitmap(const EpdFontData* fontData, const EpdGlyph* glyph, uint32_t glyphIndex);

  // Free all cached data (page buffer + hot group).
  void clearCache();

  // Pre-scan UTF-8 text and extract needed glyph bitmaps into a flat page buffer.
  // Each group is decompressed once into a temp buffer; only needed glyphs are kept.
  // Returns the number of glyphs that couldn't be loaded (0 on full success).
  int prewarmCache(const EpdFontData* fontData, const char* utf8Text);

  struct Stats {
    uint32_t cacheHits = 0;
    uint32_t cacheMisses = 0;
    uint32_t decompressTimeMs = 0;
    uint16_t uniqueGroupsAccessed = 0;
    uint32_t pageBufferBytes = 0;  // pageBuffer allocation
    uint32_t pageGlyphsBytes = 0;  // pageGlyphs lookup table allocation
    uint32_t hotGroupBytes = 0;    // current hot group allocation
    uint32_t peakTempBytes = 0;    // largest temp buffer in prewarm
    uint32_t getBitmapTimeUs = 0;  // cumulative getBitmap time (micros)
    uint32_t getBitmapCalls = 0;   // number of getBitmap calls
  };
  void logStats(const char* label = "FDC");
  void resetStats();
  const Stats& getStats() const { return stats; }

 private:
  Stats stats;
  InflateReader inflateReader;

  // Page buffer slots: each style gets its own flat glyph buffer with sorted lookup.
  // Up to MAX_PAGE_SLOTS (4) styles can be prewarmed simultaneously.
  struct PageGlyphEntry {
    uint32_t glyphIndex;
    uint32_t bufferOffset;
    uint32_t alignedOffset;  // byte-aligned offset within its decompressed group (set during prewarm pre-scan)
  };
  struct PageSlot {
    uint8_t* buffer = nullptr;
    const EpdFontData* fontData = nullptr;
    PageGlyphEntry* glyphs = nullptr;
    uint16_t glyphCount = 0;
  };
  PageSlot pageSlots[MAX_PAGE_SLOTS] = {};
  uint8_t pageSlotCount = 0;

  // ── Bounded group cache (non-prewarmed fallback path) ─────────────────────────────────────────
  //
  // getBitmap()'s fast path is the page buffer above (prewarmed, O(log n) binary-search hit). Any
  // glyph NOT prewarmed falls back here: overlay/tooltip/page-translation translated text (drawn in a
  // reader-derived font one size smaller than the body, which the held page-prewarm never covers),
  // status-bar codepoints, and first-time glyphs.
  //
  // A compressed font stores glyph bitmaps in DEFLATE groups; reading ONE glyph requires inflating
  // its whole group. The Noto built-ins put all 256 Cyrillic glyphs (U+0400-04FF) in a single group
  // (~22 KB at 12 pt, ~30 KB at 14 pt, ~38 KB at 16 pt) and ASCII/space/punctuation in a separate
  // ~6-10 KB group, so a Ukrainian translation (Latin punctuation between Cyrillic words) alternates
  // groups constantly.
  //
  // The prior code kept exactly ONE inflated group, so every transition re-inflated: each space
  // (group 0) between two Cyrillic words (group 5) evicted the Cyrillic group and the next letter
  // re-inflated it -- ~2 full inflates PER WORD. A long tooltip sentence or a full overlay page did
  // hundreds of ~30 KB inflates PER FRAME, redrawn on every step/scroll -- the reported
  // seconds-per-frame lag, correlating exactly with translated-text length. On top of that,
  // getAlignedOffset() re-scanned O(glyphIndex) glyphs on EVERY glyph (even hot-group hits).
  //
  // Fix, in three parts:
  //   1. Zero-size glyphs (space and other advance-only glyphs) short-circuit before any group work
  //      -- their bitmap is empty, so a space never touches the cache. This alone removes the most
  //      frequent evictor.
  //   2. Keep the MAX_GROUP_SLOTS most-recently-used groups (LRU). The Cyrillic group is used for
  //      every letter, so it is never the least-recently-used entry and stays pinned while the small
  //      punctuation group rides the second slot. Each needed group inflates ONCE; every subsequent
  //      draw (this frame AND later frames/steps) is a warm hit.
  //   3. Each group's per-glyph byte-aligned offsets are computed ONCE at decode time
  //      (buildGroupOffsets) so the hot path is an O(1) table lookup instead of the O(glyphIndex)
  //      getAlignedOffset scan.
  //
  // Memory: at most MAX_GROUP_SLOTS decompressed group buffers coexist, and their combined size is
  // capped at GROUP_CACHE_BUDGET_BYTES (56 KB) -- the same ceiling as the largest single group the
  // old one-slot cache could already hold, now shared by two groups so the Cyrillic + punctuation
  // working set both stay warm. Typical hold is far below the cap (~28 KB at the MEDIUM-reader / 12 pt
  // tooltip default). UI/status fonts are uncompressed (groups==nullptr) and never enter this cache.
  // Buffers are nothrow high-water malloc (NOT std::vector: getBitmap() runs on the render path and
  // under -fno-exceptions a vector resize that hits OOM abort()s the firmware -- ensureCapacity()
  // returns false on OOM so the caller skips the glyph gracefully); they are grow-only and REUSED
  // across every frame and step (no per-frame allocation), and all freed by clearCache() -- which
  // fires on overlay close, page turn, and every PrewarmScope -- so nothing outlives its render.
  struct GroupSlot {
    const EpdFontData* fontData = nullptr;  // nullptr => empty slot
    uint16_t groupIndex = UINT16_MAX;
    uint32_t firstGlyphIndex = 0;  // group base index, for the O(1) offsets[] lookup (contiguous fonts)
    uint8_t* buffer = nullptr;     // decompressed byte-aligned group (owned, grow-only)
    uint32_t bufCapacity = 0;      // allocated bytes of buffer
    uint32_t groupBytes = 0;       // uncompressedSize currently resident (0 when empty)
    uint32_t* offsets = nullptr;   // per-local-glyph aligned byte offset (owned; contiguous fonts only)
    uint16_t offsetCapacity = 0;   // entries in offsets
    bool hasOffsets = false;       // false => frequency-grouped or OOM: use getAlignedOffset fallback
    uint32_t lastUsedTick = 0;     // for LRU eviction
  };
  GroupSlot groupSlots[MAX_GROUP_SLOTS] = {};
  uint32_t usageTick = 0;  // monotonic; bumped on every group access to drive LRU

  // Scratch buffer for compacting a single glyph from a resident group.
  // Valid until the next getBitmap() call. Same ownership/OOM contract as the group buffers.
  uint8_t* hotGlyphBuf = nullptr;
  uint32_t hotGlyphBufCapacity = 0;

  // Grow (never shrink) an owned buffer to at least `needed` bytes; false on OOM, buffer freed.
  static bool ensureCapacity(uint8_t*& buf, uint32_t& capacity, uint32_t needed);

  void freePageBuffer();
  void freeGroupCache();  // frees all group slots + the glyph scratch
  // Find a resident slot holding (fontData, groupIndex), or nullptr.
  GroupSlot* findResidentGroup(const EpdFontData* fontData, uint16_t groupIndex);
  // Reserve a slot for (fontData, groupIndex): evict per budget/LRU, allocate, decompress, and
  // precompute offsets. Returns the ready slot, or nullptr on OOM/decompress failure.
  GroupSlot* acquireGroupSlot(const EpdFontData* fontData, uint16_t groupIndex, uint32_t uncompressedSize);
  // Fill slot.offsets[] for a contiguous-group font; sets hasOffsets. No-op (hasOffsets=false) for
  // frequency-grouped fonts or on OOM, so the hot path falls back to getAlignedOffset.
  void buildGroupOffsets(GroupSlot& slot, const EpdFontData* fontData, uint16_t groupIndex);

  uint16_t getGroupIndex(const EpdFontData* fontData, uint32_t glyphIndex);
  uint32_t getAlignedOffset(const EpdFontData* fontData, uint16_t groupIndex, uint32_t glyphIndex);
  bool decompressGroup(const EpdFontData* fontData, uint16_t groupIndex, uint8_t* outBuf, uint32_t outSize);
  static void compactSingleGlyph(const uint8_t* alignedSrc, uint8_t* packedDst, uint8_t width, uint8_t height);
  static int32_t findGlyphIndex(const EpdFontData* fontData, uint32_t codepoint);
};

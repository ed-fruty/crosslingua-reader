# Optimization Roadmap

Performance optimization opportunities for CrossPoint Reader on ESP32-C3 (380KB RAM).
Grouped by estimated impact. Each can be tackled independently.

---

## Tier 1 — High Impact (Noticeable Speed Improvements)

### 1. Fix font lookup copying EpdFontFamily by value in hot path

**Files:** `lib/GfxRenderer/GfxRenderer.cpp` (~line 107, 771), `lib/GfxRenderer/GfxRenderer.h` (line 39)

`const auto font = fontMap.at(fontId)` copies the entire font family struct on every `drawText` call (called per-word, thousands of times per page).

**Fix:**
- Change to `const auto& font = fontMap.at(fontId)` (add `&`)
- Replace `std::map<int, EpdFontFamily> fontMap` with a flat array — font IDs are small sequential ints, so array lookup is O(1) vs O(log n) tree traversal. Also eliminates ~40 bytes of red-black tree node overhead per entry.

### 2. Merge triple anti-aliasing render pass into single pass

**Files:** `src/activities/reader/EpubReaderActivity.cpp` (lines 732-773), `lib/GfxRenderer/`

When anti-aliasing is enabled, each page renders 3 times (BW, grayscale LSB, grayscale MSB). Each pass traverses every glyph pixel identically — only the target buffer differs.

**Fix:** Merge into a single pass that writes BW, LSB, and MSB buffers simultaneously. Could cut render time by ~60% for anti-aliased pages.

### 3. Persist ZipFile instance in Epub class

**File:** `lib/Epub/Epub.cpp` (lines 695-708)

Every `readItemContentsToStream()` / `getItemSize()` call constructs a new `ZipFile`, which re-scans the EOCD record and central directory from the SD card. During book indexing this happens dozens of times.

**Fix:** Make `ZipFile` a persistent member of `Epub` that stays open during multi-file operations. The class already has `open()`/`close()` and caches zip details.

### 4. Cache section page offset LUT and keep file handle open

**File:** `lib/Epub/Epub/Section.cpp` (lines 252-268)

Every page turn opens the section file, reads the page offset LUT, seeks to the page data, deserializes, then closes the file.

**Fix:** Cache the LUT in a `std::vector<uint32_t>` (a few hundred bytes for most chapters) and keep the file handle open while the section is active. Eliminates 3 SD seeks per page turn.

### 5. Optimize drawPixel / fillRect — bulk operations instead of per-pixel

**File:** `lib/GfxRenderer/GfxRenderer.cpp` (lines 52-74, 242-246)

Every pixel goes through `rotateCoordinates()` (switch), bounds check, byte index calculation, and bit manipulation. ~300K calls per page render. `fillRect` also draws pixel-by-pixel via `drawLine`.

**Fix:**
- For text rendering: pre-compute physical starting coordinates once per glyph, use direct buffer writes with pre-computed byte offsets. Bounds check once per glyph bounding box.
- For `fillRect` horizontal fills: compute byte range and use `memset`/byte-level operations instead of per-pixel bit toggling.

---

## Tier 2 — Medium Impact (Memory Savings & Reduced Latency)

### 6. Replace std::list with std::vector in TextBlock

**File:** `lib/Epub/Epub/blocks/TextBlock.h` (lines 15-17)

`words`, `wordXpos`, `wordStyles` use `std::list` — 8 bytes of prev/next pointer overhead per node (32-bit system). For a page with hundreds of words, this wastes KBs of RAM and hurts cache locality.

**Fix:** Change to `std::vector` for all three members. `TextBlock` (the deserialized form) doesn't use `splice`. Note: `ParsedText` (lines 17-19) also uses `std::list` for the `splice` operation in `extractLine()` — that requires a different approach (deque or index-based extraction).

### 7. Replace shared_ptr with unique_ptr for Page elements

**Files:** `lib/Epub/Epub/Page.h` (line 57), `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (lines 267, 745)

`Page::elements` uses `std::vector<std::shared_ptr<PageElement>>` but elements are only ever owned by one page. `shared_ptr` adds a control block heap allocation + atomic refcount overhead per element.

**Fix:** Change to `std::vector<std::unique_ptr<PageElement>>` and update `make_shared` to `make_unique` at call sites.

### 8. Binary search for HTML entity lookup

**File:** `lib/Epub/Epub/htmlEntities.cpp` (lines 66-76)

Linear scan of ~170 entities with `strlen` + `memcmp` per entry. Called for every HTML entity in the document.

**Fix:** Sort the `ENTITY_LOOKUP` array at compile time and use binary search — O(log 170) ≈ 8 comparisons instead of up to 170.

### 9. Skip boot screen for resume-to-reader

**File:** `src/main.cpp` (lines 290-310)

When waking from sleep to resume the last book, the boot screen is drawn then immediately replaced by the reader. This wastes ~300ms of e-ink refresh.

**Fix:** Check `APP_STATE` before drawing the boot screen. If resuming to reader, skip `BootActivity` entirely.

### 10. Remove unnecessary delay(50) after image extraction

**File:** `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` (line 218)

50ms delay per image "to give SD card time to sync" after `flush()` + `close()` — which should already ensure data is written. A chapter with 10 images loses 500ms during indexing.

**Fix:** Test without the delay. If needed, reduce to `delay(5)` or `delay(10)`.

---

## Tier 3 — Low Impact (Minor Cleanups)

### 11. Move setFadingFix / setColorTextGrayLevel out of main loop

**File:** `src/main.cpp` (lines 323-324)

These are called every loop iteration (~100/sec) but settings only change when the user explicitly edits them.

**Fix:** Call only when settings change (e.g., after returning from the settings or reader menu).

### 12. Cache hasImages() result in Page during deserialization

**File:** `lib/Epub/Epub/Page.h` (lines 63-66)

`hasImages()` iterates all elements via `std::any_of` on every page turn.

**Fix:** Add `bool containsImages = false` member, set it during `deserialize()` when a `TAG_PageImage` is encountered.

### 13. Reduce char name[500] to char name[256] in MyLibraryActivity

**File:** `src/activities/home/MyLibraryActivity.cpp` (line 81)

500 bytes on stack per iteration. SdFat filenames are max 255 bytes.

**Fix:** Change to `char name[256]`.

### 14. Avoid double-allocation for cover page HTML

**File:** `lib/Epub/Epub.cpp` (lines 86-89)

Cover page HTML is malloc'd into a buffer, then copied into a `std::string`, then the buffer is freed. Double allocation of potentially several KB.

**Fix:** Search the raw `uint8_t*` buffer directly with `memmem`-style searching instead of constructing a `std::string`.

---

## Verification (for any optimization)

1. `pio run` — must compile cleanly
2. Test page turning speed (serial log: `"Rendered page in %dms"`)
3. Monitor RAM via serial: `"Free: %d bytes"` logs every 10s
4. Test with anti-aliasing on and off
5. Test book opening (first time + cached)
6. Verify no rendering glitches across all 4 orientations

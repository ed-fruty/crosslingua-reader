# Full Book Translation — Design Spec

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Goal

Add a "Translate Book" feature that translates all chapters of an EPUB sequentially, reusing the existing streaming translation engine. This is for offline reading preparation — the user translates the entire book while they have WiFi, then reads with tooltips/side-by-side later without internet.

## Architecture

New `BookTranslatorActivity` activity that loops through all spine items, calling `TranslatingHtmlRewriter::rewriteFromFile()` for each chapter. Same blocking UI pattern as `ChapterTranslatorActivity`. WiFi connects once, stays on for the entire book. Memory stays flat (~7KB peak per chapter) because each chapter is processed and closed before the next starts.

## State Machine

```
SOURCE_LANG_SELECTION → LANG_SELECTION → CONFIRM_RETRANSLATE (conditional) → WIFI_SELECTION → TRANSLATING → DONE / FAILED / CANCELLED
```

### States

**SOURCE_LANG_SELECTION**: Launch `LanguagePickerActivity` with "Auto Detect" option for source language. On selection, save to `SETTINGS.sourceLanguage` and advance to LANG_SELECTION.

**LANG_SELECTION**: Launch `LanguagePickerActivity` for target language. On selection, save to `SETTINGS.translationLanguage` and advance to pre-translation scan.

**Pre-translation scan** (inline, not a separate state): Loop through all spine items, check `Storage.exists()` for each `{cachePath}/sections/{spineIndex}.translated.html`. Count how many already exist. If any exist, advance to CONFIRM_RETRANSLATE. Otherwise, advance directly to WIFI_SELECTION.

**CONFIRM_RETRANSLATE**: Display "X/Y chapters already translated." with two options rendered as a simple menu:
- "Re-translate All" → set `skipTranslated = false`, advance to WIFI_SELECTION
- "Skip Translated" → set `skipTranslated = true`, advance to WIFI_SELECTION
- Back button → CANCELLED

**WIFI_SELECTION**: Same as `ChapterTranslatorActivity`:
- `WiFi.mode(WIFI_STA)`
- If already connected, skip to TRANSLATING
- Otherwise launch `WifiSelectionActivity`
- On connected: configure Google DNS (8.8.8.8, 8.8.4.4), advance to TRANSLATING
- On failed: advance to FAILED

**TRANSLATING**: Create FreeRTOS task running `runTranslation()` (see Core Loop below). Display progress UI. Poll `taskDone`/`taskFailed`/`cancelFlag` in `loop()`.

**DONE**: Display "Book translation complete. X/Y chapters translated. Press any button to continue." Wait for any button press, then call `onComplete()`.

**FAILED**: Display "Translation failed at chapter X/Y. {errorDetail}. Press any button to continue." Wait for button, call `onCancel()`. Already-completed chapters keep their `.translated.html`.

**CANCELLED**: Display "Translation cancelled. X/Y chapters completed. Press any button to continue." Wait for button, call `onCancel()`. Same preservation — completed chapters not deleted.

## Core Translation Loop (FreeRTOS task)

```
runTranslation():
  Configure DNS (Google 8.8.8.8, 8.8.4.4)
  
  for spineIndex = 0 to spineCount - 1:
    if cancelFlag → break
    
    translatedPath = {cachePath}/sections/{spineIndex}.translated.html
    tempPath = {cachePath}/.tmp_book_{spineIndex}.html
    
    // Skip if already translated and user chose "Skip Translated"
    if skipTranslated && Storage.exists(translatedPath) → 
      chaptersSkipped++
      continue
    
    // Extract chapter HTML from EPUB to temp file
    href = epub->getSpineItem(spineIndex).href
    epub->readItemContentsToStream(href, tempFile, 1024)
    
    // Count blocks for progress bar
    progressTotal = TranslatingHtmlRewriter::countBlocksInFile(tempPath)
    progressCurrent = 0
    
    // Translate (streaming, file-to-file)
    TranslatingHtmlRewriter rewriter(engine, apiKey)
    Result result = rewriter.rewriteFromFile(
      tempPath, outFile, sourceLang, targetLang,
      engine, apiKey, cancelFlag, progressCurrent)
    
    // Cleanup temp
    Storage.remove(tempPath)
    
    // Handle result
    if cancelFlag →
      Storage.remove(translatedPath)  // delete incomplete
      break
    
    if result.abortedOnErrors →
      Storage.remove(translatedPath)
      taskFailed = true
      snprintf(errorDetail, "Chapter %d: %s", spineIndex, result.errorDetail)
      return
    
    chaptersCompleted++
    currentChapter++  // update progress display
  
  taskDone = true
```

### Memory Profile

Per-chapter translation (same as ChapterTranslatorActivity):
- FreeRTOS task stack: 10240 bytes
- Expat parser: ~1KB read buffer
- TranslatingHtmlRewriter: ~1.5KB batch buffer + pendingHtml strings
- Temp file I/O: streaming, no full-file loading
- **Peak: ~7KB per chapter, released between chapters**

No accumulation across chapters. Chapter N's rewriter state is fully destroyed before chapter N+1 begins.

### Error Handling

- **API errors within a chapter**: TranslatingHtmlRewriter handles retries with 100ms/2000ms delays. After MAX_CONSECUTIVE_FAILURES (20), the chapter aborts.
- **Chapter abort**: Whole book translation stops (fail fast). Incomplete `.translated.html` deleted. Error detail shown.
- **WiFi disconnect during translation**: API calls fail, consecutive failure count increases, eventually triggers abort.
- **Cancel**: User presses Back/ESC during TRANSLATING state. Sets `cancelFlag`. Current chapter's incomplete file deleted. Completed chapters preserved.

## Progress UI (render)

Two-level progress display during TRANSLATING state:

```
Line 1: "Translating Book"
Line 2: "Chapter {currentChapter}/{totalChapters}"
Line 3: Progress bar (paragraph-level within current chapter)
Line 4: "{paragraphsCurrent}/{paragraphsTotal} paragraphs"
```

Uses `requestUpdate()` on progress changes, same pattern as ChapterTranslatorActivity (throttled to avoid excessive redraws — check every 500ms in loop()).

## Menu Integration

Add "Translate Book" menu item in `EpubReaderMenuActivity`, right after the existing "Translate Chapter" item.

### I18n Strings Needed

- `STR_TRANSLATE_BOOK`: "Translate Book"
- `STR_TRANSLATING_BOOK`: "Translating Book"
- `STR_BOOK_TRANSLATION_DONE`: "Book translation complete"
- `STR_BOOK_TRANSLATION_FAILED`: "Translation failed"
- `STR_BOOK_TRANSLATION_CANCELLED`: "Translation cancelled"
- `STR_CHAPTERS_TRANSLATED`: "%d/%d chapters translated"
- `STR_RETRANSLATE_ALL`: "Re-translate All"
- `STR_SKIP_TRANSLATED`: "Skip Translated"
- `STR_CHAPTERS_ALREADY_TRANSLATED`: "%d/%d chapters already translated"

## Launch Path

In `EpubReaderMenuActivity`:
1. User selects "Translate Book" from reader menu
2. Menu calls `onTranslateBook()` callback
3. `EpubReaderActivity` creates `BookTranslatorActivity` with epub reference and callbacks
4. BookTranslatorActivity runs through state machine
5. On completion: `section->clearCache()`, `section.reset()`, `requestUpdate()` (same as chapter translator)

In `EpubReaderActivity`:
- Wire up `onTranslateBook` callback alongside existing `onTranslateChapter`
- `BookTranslatorActivity` constructor takes: `epub` (shared_ptr), `onCancel` callback, `onComplete` callback
- Does NOT take spineIndex (translates all chapters)

## Cache Invalidation

**On DONE/CANCELLED return to reader:**
1. `section->clearCache()` — current chapter `.bin` deleted
2. `section.reset()` — force reload
3. `requestUpdate()` — re-render

Other chapters' `.bin` caches rebuild lazily when the user navigates to them. `Section::loadSectionFile()` detects `.translated.html` exists and uses it as source for the new `.bin`.

## Files to Create/Modify

### New Files
- `src/activities/reader/BookTranslatorActivity.h`
- `src/activities/reader/BookTranslatorActivity.cpp`

### Modified Files
- `src/activities/reader/EpubReaderActivity.h` — add `onTranslateBook` callback, BookTranslatorActivity include
- `src/activities/reader/EpubReaderActivity.cpp` — wire up callback, create BookTranslatorActivity
- `src/activities/reader/EpubReaderMenuActivity.h` — add "Translate Book" menu item
- `src/activities/reader/EpubReaderMenuActivity.cpp` — add menu item handler
- `src/main.cpp` — wire `onTranslateBook` if needed (check if menu callbacks go through main)
- `lib/I18n/translations/*.yaml` (all 8 files) — add new strings
- `lib/I18n/I18nKeys.h` (generated) — via gen_i18n.py

### Reference Files (read, don't modify)
- `src/activities/reader/ChapterTranslatorActivity.h/.cpp` — pattern to follow
- `src/translator/TranslatingHtmlRewriter.h/.cpp` — translation engine to reuse
- `src/activities/network/WifiSelectionActivity.h/.cpp` — WiFi management pattern

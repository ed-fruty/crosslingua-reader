# CrossPoint Reader — Session Memory

## Project
ESP32-C3 e-ink reader firmware. Build: `pio run`. PlatformIO + Arduino framework, C++20.
Key constraint: 380KB usable RAM. Desktop IDE shows false-positive errors (Arduino headers unavailable on host).

## Architecture patterns
- Activities: lifecycle `onEnter/loop/render/onExit`. Navigation via function-pointer callbacks (not a stack).
- `ActivityWithSubactivity` for overlay screens; `enterNewActivity()` / `exitActivity()` to manage sub.
- Long operations (WiFi, HTTP) run in FreeRTOS tasks (4–8KB stack). `xSemaphoreCreateMutex` for shared state.
- WiFi: check `WiFi.status() == WL_CONNECTED` first; if not, launch `WifiSelectionActivity` as subactivity.
- Settings: `CrossPointSettings` singleton via `SETTINGS` macro. All fields are `uint8_t` or `char[]`.
- i18n: add strings to all 8 YAML files in `lib/I18n/translations/`, then run `python3 scripts/gen_i18n.py`.

## Key files
- `src/main.cpp` — navigation wiring, all `onGoTo*` functions
- `src/activities/reader/EpubReaderActivity.cpp` — EPUB reader main loop
- `src/activities/reader/EpubReaderMenuActivity.h/.cpp` — reader menu (add `MenuAction` enum + menu item vector)
- `src/activities/reader/ReaderActivity.h/.cpp` — wraps EPUB/XTC/TXT readers, dispatches by file type
- `lib/Epub/Epub.h` — EPUB metadata/spine loader (read-only)
- `lib/ZipFile/ZipFile.h` — read-only ZIP wrapper (uses miniz internally)
- `lib/miniz/miniz.h` — full ZIP read+write API (writer unused until translation feature)
- `src/network/HttpDownloader.h` — static HTTP/HTTPS fetch utility

## Translation feature (feat/epub-translator branch)
Added EPUB bilingual translation via Google Translate free API.
New files:
- `src/translator/ParagraphTranslator.h/.cpp` — Google Translate gtx API caller + JSON parser
- `src/translator/TranslatingHtmlRewriter.h/.cpp` — expat SAX rewriter; inserts `<p style="color:#5A5A5A">` after each block element
- `src/translator/EpubTranslator.h/.cpp` — FreeRTOS task; opens source EPUB with miniz custom read callbacks, creates output EPUB with miniz custom write callbacks, rewrites HTML chapters, copies assets via `mz_zip_writer_add_from_zip_reader`
- `src/activities/translator/LanguagePickerActivity.h/.cpp` — language list picker
- `src/activities/translator/TranslatorActivity.h/.cpp` — progress screen, state machine (LANG_SELECTION → WIFI_SELECTION → TRANSLATING → DONE/FAILED/CANCELLED)

Modified files:
- `EpubReaderMenuActivity.h` — added `TRANSLATE_BOOK` MenuAction + menu item `STR_TRANSLATE_BOOK`
- `EpubReaderActivity.h/.cpp` — added `onGoToTranslator` callback, `pendingTranslate` flag, `TRANSLATE_BOOK` case
- `ReaderActivity.h/.cpp` — threads `onGoToTranslator` callback through to EpubReaderActivity
- `main.cpp` — added `onGoToTranslator()` function, added include for TranslatorActivity
- All 8 translation YAML files — added 8 new STR_* keys (STR_TRANSLATE_BOOK, etc.)

## miniz ZIP writer with FsFile
Custom read/write callbacks (see EpubTranslator.cpp):
```cpp
srcZip.m_pRead = zipReadFunc;   // size_t(void* opaque, mz_uint64 ofs, void* buf, size_t n)
dstZip.m_pWrite = zipWriteFunc; // size_t(void* opaque, mz_uint64 ofs, const void* buf, size_t n)
mz_zip_reader_init(&srcZip, srcFile.size(), 0);
mz_zip_writer_init(&dstZip, 0);
mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, fileIndex); // efficient asset copying
```
mimetype must be first + use `MZ_NO_COMPRESSION`.

## Build status
- RAM: ~31% at rest, adequate for translation workload
- Flash: ~66%

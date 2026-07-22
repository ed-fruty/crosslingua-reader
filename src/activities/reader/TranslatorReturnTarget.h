#pragma once
#include <cstdint>

// Where a translator activity should return once it finishes (or is cancelled).
//
// READER (the default) reproduces the classic reader flow: the EPUB reader tore
// itself down before launching the translator, so the translator relaunches it
// from disk via ActivityManager::goToReader(epubPath).
//
// FILE_BROWSER is used when Pre-Translation was started straight from the file
// browser (no reader was ever open). The translator returns to the file browser
// at the book's parent directory via ActivityManager::goToFileBrowser(dir).
//
// Declared once here and shared by ChapterTranslatorActivity and
// BookTranslatorActivity so both accept the same trailing ctor parameter.
enum class TranslatorReturnTarget : uint8_t {
  READER = 0,
  FILE_BROWSER = 1,
};

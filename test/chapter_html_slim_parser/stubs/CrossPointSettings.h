#pragma once

#include <cstdint>

// Host stub for the Lingua sidecar tests: only the engine ids TranslationEnginePolicy switches on.
class CrossPointSettings {
 public:
  enum TRANSLATION_ENGINE : uint8_t {
    ENGINE_GOOGLE = 0,
    ENGINE_OPENAI = 1,
    ENGINE_DEEPSEEK = 2,
    ENGINE_GEMINI = 3,
    ENGINE_AZURE = 8,
  };
};

struct DictionarySettingsStub {
  bool extraParagraphSpacing = false;
  uint8_t paragraphAlignment = 0;
  bool hyphenationEnabled = false;
  bool focusReadingEnabled = false;
  uint8_t wordSpacing = 100;
  uint8_t paragraphIndentSpaces = 0;
  int getReaderFontId() const { return 0; }
  float getReaderLineCompression() const { return 1.0f; }
  int8_t getCharacterSpacing() const { return 0; }
};

inline DictionarySettingsStub SETTINGS;

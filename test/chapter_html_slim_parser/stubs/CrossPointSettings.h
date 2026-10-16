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

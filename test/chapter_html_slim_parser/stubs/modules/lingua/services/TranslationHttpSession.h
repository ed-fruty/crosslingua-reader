#pragma once

#include <cstdint>

#include "modules/lingua/services/TranslatorHttp.h"

// Host stub: no network, the heap is always ready.
struct TranslationHttpSession {
  bool waitForHeapReady(uint32_t, volatile const bool*) { return true; }
};

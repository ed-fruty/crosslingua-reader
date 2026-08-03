#include "TranslationEnginePolicy.h"

#include "CrossPointSettings.h"

namespace {
constexpr uint32_t DEFAULT_RATE_LIMIT_BACKOFF_MS[] = {1500};
constexpr uint32_t EDGE_RATE_LIMIT_BACKOFF_MS[] = {5000, 15000, 30000, 60000};

constexpr TranslationEnginePolicy DEFAULT_POLICY = {
    TranslationBatchMode::None,
    1500,
    1800,
    2,
    3,
    DEFAULT_RATE_LIMIT_BACKOFF_MS,
    1,
};

constexpr TranslationEnginePolicy SEPARATOR_BATCH_POLICY = {
    TranslationBatchMode::SeparatorDelimited,
    1500,
    1800,
    2,
    3,
    DEFAULT_RATE_LIMIT_BACKOFF_MS,
    1,
};

// The Edge deployment accepts an array of source texts. Its rate limiter may
// close a longer window than ordinary transient HTTP failures, so retries use
// a bounded cooldown instead of immediately repeating the same burst.
constexpr TranslationEnginePolicy EDGE_NATIVE_BATCH_POLICY = {
    TranslationBatchMode::NativeArray,
    1500,
    1800,
    5,
    5,
    EDGE_RATE_LIMIT_BACKOFF_MS,
    sizeof(EDGE_RATE_LIMIT_BACKOFF_MS) / sizeof(EDGE_RATE_LIMIT_BACKOFF_MS[0]),
};
}  // namespace

uint32_t TranslationEnginePolicy::retryDelayMs(int httpCode, size_t retryIndex) const {
  if (httpCode == 429) {
    if (rateLimitBackoffCount == 0) return 1500;
    if (retryIndex >= rateLimitBackoffCount) retryIndex = rateLimitBackoffCount - 1;
    return rateLimitBackoffMs[retryIndex];
  }

  static constexpr uint32_t TRANSIENT_BACKOFF_MS[] = {500, 1500, 3000};
  constexpr size_t count = sizeof(TRANSIENT_BACKOFF_MS) / sizeof(TRANSIENT_BACKOFF_MS[0]);
  if (retryIndex >= count) retryIndex = count - 1;
  return TRANSIENT_BACKOFF_MS[retryIndex];
}

const TranslationEnginePolicy& translationEnginePolicy(uint8_t engine) {
  switch (engine) {
    case CrossPointSettings::ENGINE_OPENAI:
    case CrossPointSettings::ENGINE_DEEPSEEK:
    case CrossPointSettings::ENGINE_GEMINI:
      return SEPARATOR_BATCH_POLICY;
    case CrossPointSettings::ENGINE_AZURE:
      return EDGE_NATIVE_BATCH_POLICY;
    default:
      return DEFAULT_POLICY;
  }
}

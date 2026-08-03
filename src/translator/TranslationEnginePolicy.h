#pragma once

#include <cstddef>
#include <cstdint>

enum class TranslationBatchMode : uint8_t {
  None,
  SeparatorDelimited,
  NativeArray,
};

struct TranslationEnginePolicy {
  TranslationBatchMode batchMode;
  size_t batchTargetBytes;
  size_t maxMergedTextBytes;
  uint8_t maxAttempts;
  uint8_t maxConsecutiveRateLimits;
  const uint32_t* rateLimitBackoffMs;
  size_t rateLimitBackoffCount;

  bool supportsBatching() const { return batchMode != TranslationBatchMode::None; }
  bool requiresBoundedBatch() const { return batchMode == TranslationBatchMode::NativeArray; }
  uint32_t retryDelayMs(int httpCode, size_t retryIndex) const;
};

const TranslationEnginePolicy& translationEnginePolicy(uint8_t engine);

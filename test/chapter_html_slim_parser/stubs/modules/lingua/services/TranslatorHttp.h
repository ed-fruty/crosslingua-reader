#pragma once

// Host stub: the rewriter only reads the last HTTP status for its retry policy.
struct TranslatorHttp {
  static inline int lastHttpCode = 200;
};

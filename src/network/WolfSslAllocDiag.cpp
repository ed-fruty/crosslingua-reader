#include "WolfSslAllocDiag.h"

#if defined(FREEINK_NET_WOLFSSL)

#include <Arduino.h>
#include <Logging.h>
#include <wolfssl/wolfcrypt/memory.h>

#include <cstdlib>
#include <type_traits>

// This wrapper assumes the plain callback signatures (no per-heap/per-type
// bucket, no caller file/line). Both extra forms are opt-in via these two
// macros, which are not defined anywhere in platformio.ini or
// scripts/patch_wolfssl.py for this build (see the design doc's evidence
// index). If either is ever enabled, the callback signatures below must be
// updated to match wolfssl/wolfcrypt/memory.h's WOLFSSL_STATIC_MEMORY /
// WOLFSSL_DEBUG_MEMORY variants.
#if defined(WOLFSSL_STATIC_MEMORY) || defined(WOLFSSL_DEBUG_MEMORY)
#error \
    "WolfSslAllocDiag assumes the plain wolfSSL_Malloc_cb/Free_cb/Realloc_cb signatures; update the callbacks in WolfSslAllocDiag.cpp for the enabled memory macro."
#endif

namespace {

// Pass-through malloc: identical semantics to plain malloc(), but logs the
// requested size and current heap headroom on failure so an on-device OOM
// during a TLS handshake names the exact failing allocation.
void* diagMalloc(size_t size) {
  void* ptr = malloc(size);
  if (!ptr && size > 0) {
    LOG_ERR("WSSL", "XMALLOC FAIL: req=%u maxBlock=%u free=%u", (unsigned)size, (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getFreeHeap());
  }
  return ptr;
}

void* diagRealloc(void* ptr, size_t size) {
  void* newPtr = realloc(ptr, size);
  if (!newPtr && size > 0) {
    LOG_ERR("WSSL", "XREALLOC FAIL: req=%u maxBlock=%u free=%u", (unsigned)size, (unsigned)ESP.getMaxAllocHeap(),
            (unsigned)ESP.getFreeHeap());
  }
  return newPtr;
}

void diagFree(void* ptr) { free(ptr); }

// Compile-time guard: if wolfSSL's callback typedefs ever drift from the
// plain form assumed above, this fails the build instead of silently
// mismatching calling conventions at link time.
static_assert(std::is_same<decltype(&diagMalloc), wolfSSL_Malloc_cb>::value,
              "diagMalloc signature no longer matches wolfSSL_Malloc_cb");
static_assert(std::is_same<decltype(&diagFree), wolfSSL_Free_cb>::value,
              "diagFree signature no longer matches wolfSSL_Free_cb");
static_assert(std::is_same<decltype(&diagRealloc), wolfSSL_Realloc_cb>::value,
              "diagRealloc signature no longer matches wolfSSL_Realloc_cb");

}  // namespace

void installWolfSslAllocDiag() {
  wolfSSL_SetAllocators(diagMalloc, diagFree, diagRealloc);
  LOG_INF("WSSL", "Diagnostic allocator installed");
}

#else  // !FREEINK_NET_WOLFSSL

void installWolfSslAllocDiag() {}

#endif

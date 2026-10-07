#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "network/HttpDownloader.h"

/**
 * HTTP(S) client for the Lingua translation engines (GET to a string, POST,
 * keep-alive session). This is the pre-1.5 HttpDownloader request flow, kept
 * as a fork-owned copy so upstream changes to HttpDownloader (used by OPDS,
 * fonts, plugins and OTA) do not alter the translation path.
 */
class TranslatorHttp {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  // TLS heap floors, measured on-device via WolfSslAllocDiag (see the rationale
  // in TranslatorHttp.cpp). Exposed so the translation activities can apply the
  // SAME backpressure thresholds before starting a chapter's handshake as the
  // request-time guards enforce, keeping the numbers single-sourced.
  //   MIN_FREE_HEAP_FOR_TLS   total headroom for the handshake's transient peak.
  //   MIN_MAX_ALLOC_FOR_TLS   largest contiguous block for the worst-case TLS record.
  //   MIN_FREE_HEAP_FOR_REUSE small floor for a request on an already-handshaken socket.
  //
  // The contiguous-block floor matches KOSync's MIN_BLOCK_FOR_TLS (both sized for the
  // ~17 KB wolfSSL record buffer). The free-heap floor sits above upstream's download
  // pre-flight (HttpDownloader::MIN_TLS_FREE_HEAP): a download that OOMs mid-stream resumes, but
  // the translator runs hundreds of handshakes back to back and a MEMORY_E counts toward the
  // low-memory stop, so it keeps margin over the measured handshake cost (see TranslatorHttp.cpp).
  static constexpr uint32_t MIN_FREE_HEAP_FOR_TLS = 45000;
  static constexpr uint32_t MIN_MAX_ALLOC_FOR_TLS = 20000;
  static constexpr uint32_t MIN_FREE_HEAP_FOR_REUSE = 12000;

  /**
   * Fetch text content from a URL with optional credentials.
   * @param userAgent Overrides the default "CrossPoint-ESP32-<version>" User-Agent for
   *        this call only; nullptr keeps the default. Needed for endpoints that are
   *        UA-sensitive (the anonymous Azure/Edge token endpoint expects a browser UA).
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "", const char* userAgent = nullptr);

  /**
   * POST with configurable content type and a custom header.
   * @param url The URL to POST to
   * @param body The request body
   * @param contentType Content-Type header value (e.g. "application/json+protobuf"), nullptr to skip
   * @param extraHeaderName Extra header name (e.g. "X-Goog-Api-Key"), nullptr to skip
   * @param extraHeaderValue Extra header value
   * @param outContent The response body (output)
   * @return true if request succeeded (HTTP 200), false on error
   */
  static bool post(const std::string& url, const std::string& body, const char* contentType,
                   const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent);

  /**
   * POST JSON to a URL and return the response body.
   * @param url The URL to POST to
   * @param jsonBody The JSON request body
   * @param authHeader Authorization header value (e.g. "Bearer xxx"), empty to skip
   * @param outContent The response body (output)
   * @return true if request succeeded (HTTP 200), false on error
   */
  static bool postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                       std::string& outContent);

  // Last HTTP response code from the most recent post/postJson call. Negative
  // values indicate connection-level failures (bad URL, transport error, or the
  // pre-request low-heap guard); positive values are HTTP status codes. Set on
  // every exit path, including connection-level failures. Used by callers
  // (e.g. ParagraphTranslator) to surface engine-specific errors to the user.
  static int lastHttpCode;
};

/**
 * Reusable HTTP(S) session for a burst of requests to the same origin.
 *
 * The TranslatorHttp statics create (and tear down) a fresh client — and, on
 * the wolfSSL build, a full TLS handshake — on every call. A chapter
 * translation fires one request per paragraph/batch to the SAME host, so that
 * meant one ~1.4 s handshake per paragraph, and the per-handshake heap churn
 * fragmented the ~70 KB pool until TLS 1.3's single large cert-flight
 * allocation (see diagnosis) began missing the largest contiguous block.
 *
 * This owns ONE freeink::SecureHttpClient with keep-alive enabled, so a whole
 * chapter pays for a single handshake. The request methods mirror the matching
 * TranslatorHttp static contracts exactly (same lastHttpCode semantics — GET
 * leaves it untouched, POST/postJson set it on every exit path; same insecure
 * mode; UA via setUserAgent, never a duplicate User-Agent header). If the
 * kept-alive socket died between requests, SecureHttpClient's own one-shot
 * stale-retry transparently reconnects.
 *
 * RAII: construct one on the stack for the lifetime of the burst; the
 * destructor closes the connection. Non-copyable (owns a live connection).
 * On the non-wolfSSL (esp_http_client) build, and if the internal client
 * cannot be allocated, every method transparently delegates to the matching
 * TranslatorHttp static — behavior is then identical to not using a session.
 */
class ReusableHttpSession {
 public:
  ReusableHttpSession();
  ~ReusableHttpSession();
  ReusableHttpSession(const ReusableHttpSession&) = delete;
  ReusableHttpSession& operator=(const ReusableHttpSession&) = delete;

  // Mirror of TranslatorHttp::fetchUrl(url, std::string&): GET, buffered body,
  // follows redirects, true only on HTTP 200 with a complete body. Does NOT
  // touch TranslatorHttp::lastHttpCode (matching the static GET path).
  bool fetchUrl(const std::string& url, std::string& outContent);

  // Mirror of TranslatorHttp::post(...): sets TranslatorHttp::lastHttpCode on
  // every exit path (negative on connection-level failure, else HTTP status).
  bool post(const std::string& url, const std::string& body, const char* contentType, const char* extraHeaderName,
            const char* extraHeaderValue, std::string& outContent);

  // Mirror of TranslatorHttp::postJson(...); delegates to post().
  bool postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                std::string& outContent);

  // Heap backpressure before a request's TLS (re)connect. Blocks (polling, with a
  // watchdog-feeding delay) until the heap can support the applicable floor — the
  // full handshake floor (MIN_FREE_HEAP_FOR_TLS / MIN_MAX_ALLOC_FOR_TLS) before the
  // first request, the small reuse floor (MIN_FREE_HEAP_FOR_REUSE) once the socket
  // is established — or until timeoutMs elapses or *cancelFlag (nullable) is set.
  // Returns true when the heap is ready OR when no guard applies (non-wolfSSL build /
  // no session), so the caller should proceed; false only on a genuine timeout or
  // cancel, which the caller counts toward its low-memory termination contract
  // instead of firing a request that would OOM. Never issues a request itself.
  bool waitForHeapReady(uint32_t timeoutMs, volatile const bool* cancelFlag);

 private:
  struct Impl;                 // owns the kept-alive client (wolfSSL build only)
  std::unique_ptr<Impl> impl;  // null on the non-wolfSSL build or on OOM -> delegate to statics
};

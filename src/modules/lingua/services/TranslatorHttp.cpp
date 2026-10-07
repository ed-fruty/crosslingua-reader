// CrossLingua translator networking, kept separate from upstream's
// HttpDownloader so upstream download changes do not alter the translation
// path unless chosen. CROSSLINGUA_TRANSLATOR_HTTP_UPSTREAM (platformio.ini
// [crosslingua] build_flags) selects the GET backend:
//   1  GET over upstream's freeink::fetchResumable (Range resume, origin-scoped
//      credentials) with upstream's WifiPowerSaveGuard;
//   0  the pre-1.5 HttpDownloader flow (manual redirect loop, heap check per hop),
//      moved here verbatim except for the class name and the wolfSSL debug hook,
//      which upstream's HttpDownloader.cpp still defines.
// POST, the keep-alive session and the TLS heap pre-checks are shared.
#include "modules/lingua/services/TranslatorHttp.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_wifi.h>

#include <functional>
#include <string>

#include "modules/lingua/services/HeapBackpressure.h"

#if defined(CROSSLINGUA_TRANSLATOR_HTTP_UPSTREAM) && CROSSLINGUA_TRANSLATOR_HTTP_UPSTREAM
#define TRANSLATOR_HTTP_UPSTREAM 1
#if !defined(FREEINK_NET_WOLFSSL)
#error "CROSSLINGUA_TRANSLATOR_HTTP_UPSTREAM needs the wolfSSL stack (FREEINK_NET_WOLFSSL)"
#endif
#include <ResumableFetch.h>

#include "network/WifiPowerSaveGuard.h"
#else
#define TRANSLATOR_HTTP_UPSTREAM 0
#endif

#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#else
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#endif

int TranslatorHttp::lastHttpCode = 0;

namespace {
#if !defined(FREEINK_NET_WOLFSSL)
// RX holds the response headers. Smaller buffers leave enough contiguous heap
// for mbedTLS on redirect-heavy OPDS feeds while still preserving the headers
// we read directly (Location, Content-Length).
constexpr int HTTP_RX_BUF = 2048;
constexpr int HTTP_TX_BUF = 512;
#endif
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 1024;
constexpr int MAX_REDIRECTS = 5;

#if !TRANSLATOR_HTTP_UPSTREAM
struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  TranslatorHttp::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// OtaUpdater.cpp already disables WiFi power-save for firmware downloads, but
// OPDS feed/book fetches never did despite being able to run just as long for
// a large category. Modem sleep periodically powers the radio down between
// DTIM beacon intervals, which can drop or stall packets mid-transfer -- more
// likely to be hit the longer a transfer takes, so small feeds mostly get
// away with it while a large category consistently doesn't.
struct WifiPowerSaveGuard {
  WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to disable WiFi power-save: %d", err);
  }
  ~WifiPowerSaveGuard() {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) LOG_ERR("HTTP", "Failed to restore WiFi power-save: %d", err);
  }
};
#endif  // !TRANSLATOR_HTTP_UPSTREAM

#if defined(FREEINK_NET_WOLFSSL)
// TLS handshakes over wolfSSL are the single biggest heap consumer on this
// path; translation requests are one-shot (no keep-alive to amortize the cost
// across requests), so refuse the request up front rather than risk an OOM
// mid-handshake.
//
// Thresholds are evidence-based from on-device WolfSslAllocDiag capture, not
// guesses: the handshake starts at ~37 KB free, churns ~24 KB across many
// small allocations, then makes ONE request for 10,460 bytes while the
// largest contiguous hole is only 6,644 bytes -> MEMORY_E. A TLS1.2 fallback
// retry compounds this: it starts at 5,252 bytes free and fails at a 2,092
// byte request, and each failed attempt further degrades maxAlloc (observed
// 30,708 -> 19,444 across retries). So the real requirement is twofold:
//   1. Enough TOTAL headroom to absorb the ~24 KB of handshake churn on top
//      of the worst-case single block (10,460 B measured, up to ~16.4 KB for
//      larger TLS records) -> freeHeap floor.
//   2. A single contiguous block large enough for that worst-case TLS record
//      allocation to actually succeed even if freeHeap looks sufficient but
//      is fragmented -> maxAllocHeap floor.
// Both floors carry margin above the measured numbers so we refuse before
// hitting the wall, not after. The threshold values live in TranslatorHttp.h
// (TranslatorHttp::MIN_*) so the translation activities can share them.
bool insufficientHeapForTls() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (freeHeap < TranslatorHttp::MIN_FREE_HEAP_FOR_TLS || maxAllocHeap < TranslatorHttp::MIN_MAX_ALLOC_FOR_TLS) {
    LOG_ERR("HTTP", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc block (need %u)",
            freeHeap, TranslatorHttp::MIN_FREE_HEAP_FOR_TLS, maxAllocHeap, TranslatorHttp::MIN_MAX_ALLOC_FOR_TLS);
    return true;
  }
  return false;
}

// Reused-connection floor: a request on an already-handshaken keep-alive socket
// does NOT pay the handshake cost — no ~24 KB of churn and no single large
// cert-flight record buffer (the ~12.5 KB GrowInputBuffer allocation happens
// only while receiving the server's certificate at handshake time). It needs
// only working room for the request line/headers and the buffered response, so
// the full TLS floor (which would wrongly refuse a reused request whenever
// maxAlloc has dropped below the handshake bar) does not apply. Gate instead on
// a small absolute floor so we still refuse before an OOM mid-request.
//
// Caveat: if the kept-alive socket has died, SecureHttpClient re-handshakes
// transparently on this same call; that re-handshake could then fail under this
// low floor. That is acceptable — it surfaces as a normal request failure that
// the caller's retry/backoff handles — and is far rarer than the common case of
// a live socket we must not needlessly refuse. The threshold lives in
// TranslatorHttp.h (TranslatorHttp::MIN_FREE_HEAP_FOR_REUSE).
bool insufficientHeapForReuse() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < TranslatorHttp::MIN_FREE_HEAP_FOR_REUSE) {
    LOG_ERR("HTTP", "Insufficient heap for reused TLS request: %u bytes free (need %u)", freeHeap,
            TranslatorHttp::MIN_FREE_HEAP_FOR_REUSE);
    return true;
  }
  return false;
}

#if !TRANSLATOR_HTTP_UPSTREAM
TranslatorHttp::DownloadError runGetWolf(const std::string& startUrl, const std::string& username,
                                         const std::string& password, Sink& sink, const char* userAgent,
                                         bool downgradeRedirectsToHttp) {
  WifiPowerSaveGuard psGuard;
  std::string url = startUrl;

  for (int hop = 0; hop <= MAX_REDIRECTS; ++hop) {
    if (insufficientHeapForTls()) return TranslatorHttp::HTTP_ERROR;

    freeink::SecureHttpClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
      return TranslatorHttp::HTTP_ERROR;
    }
    // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
    // append a second User-Agent header, which strict servers reject (aiohttp
    // answers 400 "Duplicate 'User-Agent' header found"). A caller-supplied UA
    // replaces ours the same way — still exactly one User-Agent header.
    http.setUserAgent(userAgent ? userAgent : "CrossPoint-ESP32-" CROSSPOINT_VERSION);
    if (!username.empty() && !password.empty()) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    LOG_DBG("HTTP", "wolfSSL GET: %s", url.c_str());
    const int status = http.GET(
        [&http, &sink](const uint8_t* data, size_t len) {
          if (http.getStatus() != 200) return true;
          if (sink.total == 0 && http.hasContentLength()) sink.total = http.getContentLength();
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
          return true;
        },
        [&sink]() { return sink.cancelFlag && *sink.cancelFlag; });

    if (http.aborted()) return TranslatorHttp::ABORTED;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", url.c_str());
      return TranslatorHttp::HTTP_ERROR;
    }
    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty() || !freeink::SecureHttpClient::resolveUrl(url, location, url)) {
        LOG_ERR("HTTP", "wolfSSL bad redirect: %d", status);
        return TranslatorHttp::HTTP_ERROR;
      }
      if (downgradeRedirectsToHttp && url.rfind("https://", 0) == 0) {
        // Fetch the redirect target over plain HTTP. GitHub's release-asset
        // CDN serves its signed URLs on both schemes, and skipping the second
        // TLS session removes its ~17KB record buffer — the MEMORY_E /
        // OOM-abort site on C3 heaps that sit near 45KB free.
        url.replace(0, 8, "http://");
      }
      continue;
    }
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL unexpected status: %d", status);
      return TranslatorHttp::HTTP_ERROR;
    }
    if (http.callbackAborted()) return TranslatorHttp::FILE_ERROR;
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      return TranslatorHttp::HTTP_ERROR;
    }
    return TranslatorHttp::OK;
  }
  LOG_ERR("HTTP", "too many redirects");
  return TranslatorHttp::HTTP_ERROR;
}

#endif  // !TRANSLATOR_HTTP_UPSTREAM

// POST a body and buffer the response over wolfSSL. Translation engine
// responses are small (~KB range), so buffering via SecureHttpClient's
// internal getString() is fine — no need for the streaming sink used by GET.
// Sets TranslatorHttp::lastHttpCode on every exit path, including
// connection-level failures (negative codes), so ParagraphTranslator can
// surface engine-specific errors.
bool runPostWolf(const std::string& url, const std::string& body, const char* contentType, const char* extraHeaderName,
                 const char* extraHeaderValue, std::string& outContent) {
  outContent.clear();

  if (insufficientHeapForTls()) {
    TranslatorHttp::lastHttpCode = -1;
    return false;
  }

  freeink::SecureHttpClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setInsecure();
  if (!http.begin(url)) {
    LOG_ERR("HTTP", "wolfSSL bad URL: %s", url.c_str());
    TranslatorHttp::lastHttpCode = -1;
    return false;
  }
  // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
  // append a second User-Agent header, which strict servers reject (aiohttp
  // answers 400 "Duplicate 'User-Agent' header found").
  http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (contentType && *contentType) {
    http.addHeader("Content-Type", contentType);
  }
  if (extraHeaderName && extraHeaderValue) {
    http.addHeader(extraHeaderName, extraHeaderValue);
  }

  LOG_DBG("HTTP", "wolfSSL POST: %s (body=%u bytes)", url.c_str(), (unsigned)body.size());
  const int status = http.sendRequest("POST", body);
  TranslatorHttp::lastHttpCode = status;
  if (status < 0) {
    LOG_ERR("HTTP", "wolfSSL POST failed: %s", url.c_str());
    return false;
  }
  // getString() holds the buffered response body for the lifetime of `http`;
  // copy it out before http goes out of scope.
  outContent = http.getString();
  if (status != 200) {
    LOG_ERR("HTTP", "wolfSSL POST unexpected status: %d (%u byte body)", status, (unsigned)outContent.size());
    return false;
  }
  return true;
}
#endif

#if !defined(FREEINK_NET_WOLFSSL)
// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
TranslatorHttp::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink, const char* userAgent) {
  WifiPowerSaveGuard psGuard;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = true;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    return TranslatorHttp::HTTP_ERROR;
  }

  // A caller-supplied UA replaces the default for this request only.
  esp_http_client_set_header(client, "User-Agent", userAgent ? userAgent : "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (!username.empty() && !password.empty()) {
    // Preemptive Basic auth, like the prior addHeader; don't wait for a 401.
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return TranslatorHttp::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  for (int hop = 0; isRedirect(status) && hop < MAX_REDIRECTS; ++hop) {
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    esp_http_client_close(client);
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return TranslatorHttp::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
  }

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    esp_http_client_cleanup(client);
    return TranslatorHttp::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  auto buf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!buf) {
    LOG_ERR("HTTP", "OOM: %u byte read buffer", (unsigned)READ_CHUNK);
    esp_http_client_cleanup(client);
    return TranslatorHttp::HTTP_ERROR;
  }

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      esp_http_client_cleanup(client);
      return TranslatorHttp::ABORTED;
    }
    const int read = esp_http_client_read(client, buf.get(), READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      esp_http_client_cleanup(client);
      return TranslatorHttp::HTTP_ERROR;
    }
    if (read == 0) break;  // all data received
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf.get()), read)) {
      esp_http_client_cleanup(client);
      return TranslatorHttp::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress && sink.total > 0) sink.progress(sink.downloaded, sink.total);
  }

  const bool complete = esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    return TranslatorHttp::HTTP_ERROR;
  }
  return TranslatorHttp::OK;
}

// POST a body and buffer the response. Uses esp_http_client_perform() since
// translation engine response bodies are small (~KB range) and we don't need
// to stream. Sets TranslatorHttp::lastHttpCode on every exit path.
bool runPost(const std::string& url, const std::string& body, const char* contentType, const char* extraHeaderName,
             const char* extraHeaderValue, std::string& outContent) {
  outContent.clear();

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = HTTP_METHOD_POST;
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  // Don't keep-alive: translation POSTs are one-shot, and dropping the
  // connection lets the TLS session free its buffers immediately.
  config.keep_alive_enable = false;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "POST client init failed");
    TranslatorHttp::lastHttpCode = -1;
    return false;
  }

  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  if (contentType && *contentType) {
    esp_http_client_set_header(client, "Content-Type", contentType);
  }
  if (extraHeaderName && extraHeaderValue) {
    esp_http_client_set_header(client, extraHeaderName, extraHeaderValue);
  }
  if (!body.empty()) {
    esp_http_client_set_post_field(client, body.c_str(), static_cast<int>(body.size()));
  }

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "POST perform failed: %s", esp_err_to_name(err));
    TranslatorHttp::lastHttpCode = -1;
    esp_http_client_cleanup(client);
    return false;
  }

  const int status = esp_http_client_get_status_code(client);
  TranslatorHttp::lastHttpCode = status;

  // Drain the response body. esp_http_client_perform() buffers the body
  // internally when no event callback is set; read it out via
  // esp_http_client_read_response().
  const int64_t contentLength = esp_http_client_get_content_length(client);
  auto readBuf = makeUniqueNoThrow<char[]>(READ_CHUNK);
  if (!readBuf) {
    LOG_ERR("HTTP", "OOM: POST read buffer");
    esp_http_client_cleanup(client);
    return false;
  }
  outContent.reserve(contentLength > 0 ? static_cast<size_t>(contentLength) : 1024);
  while (true) {
    const int read = esp_http_client_read_response(client, readBuf.get(), READ_CHUNK);
    if (read <= 0) break;
    outContent.append(readBuf.get(), read);
  }

  esp_http_client_cleanup(client);

  if (status != 200) {
    LOG_ERR("HTTP", "POST status %d (%u byte body)", status, (unsigned)outContent.size());
    return false;
  }
  return true;
}
#endif  // !FREEINK_NET_WOLFSSL

#if !TRANSLATOR_HTTP_UPSTREAM
// All HTTP(S) fetches go through wolfSSL when it is the active TLS stack: it
// speaks TLS 1.3 and reads large bodies from servers where the esp_http_client/
// mbedTLS path fails to connect or stalls mid-stream. Plain-http URLs still use a
// WiFiClient inside runGetWolf, so this is safe for non-TLS targets too.
// `userAgent` is nullptr for every caller but the ones that must present a specific
// User-Agent (see TranslatorHttp::fetchUrl); nullptr keeps the default CrossPoint UA.
TranslatorHttp::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password, Sink& sink, const char* userAgent = nullptr,
                                           bool downgradeRedirectsToHttp = false) {
#if defined(FREEINK_NET_WOLFSSL)
  return runGetWolf(url, username, password, sink, userAgent, downgradeRedirectsToHttp);
#else
  // esp_http_client follows redirects internally; the downgrade only exists on
  // the wolfSSL path, where the manual hop loop exposes the Location URL.
  (void)downgradeRedirectsToHttp;
  return runGet(url, username, password, sink, userAgent);
#endif
}

#endif  // !TRANSLATOR_HTTP_UPSTREAM

// Translation POST requests stay separate from upstream's download path.
bool runPostSecure(const std::string& url, const std::string& body, const char* contentType,
                   const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent) {
#if defined(FREEINK_NET_WOLFSSL)
  return runPostWolf(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
#else
  return runPost(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
#endif
}
}  // namespace

#if TRANSLATOR_HTTP_UPSTREAM
bool TranslatorHttp::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, const char* userAgent) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();
  if (insufficientHeapForTls()) return false;  // GET does not touch lastHttpCode
  WifiPowerSaveGuard psGuard;
  freeink::FetchSink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  // A server that ignores a resume Range restarts the body from byte 0.
  sink.rewind = [&outContent] {
    outContent.clear();
    return true;
  };
  // fetchResumable opens a fresh TLS connection per redirect hop and per resume. configure runs right
  // before each one, so the heap floor is re-checked there and the abort callback refuses the connect.
  // Latched rather than polled: the callback also runs mid-transfer, when the live session itself has
  // pulled the heap below the floor.
  bool lowHeapBeforeConnect = false;
  const freeink::FetchResult result = freeink::fetchResumable(
      url, freeink::FetchOptions{},
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        lowHeapBeforeConnect = insufficientHeapForTls();
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setInsecure();
        // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
        // append a second User-Agent header, which strict servers reject.
        http.setUserAgent(userAgent ? userAgent : "CrossPoint-ESP32-" CROSSPOINT_VERSION);
        if (sameOrigin && !username.empty() && !password.empty()) http.setBasicAuth(username, password);
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
      sink, [&lowHeapBeforeConnect] { return lowHeapBeforeConnect; });
  // 206 only arises from a resumed body that completed; anything else but 200 is a failure.
  if (!result.complete || (result.status != 200 && result.status != 206)) {
    LOG_ERR("HTTP", "wolfSSL GET failed: status %d, got %zu of %zu bytes: %s", result.status, result.bytes,
            result.total, url.c_str());
    return false;
  }
  return true;
}
#else
bool TranslatorHttp::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password, const char* userAgent) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGetSecure(url, username, password, sink, userAgent) == OK;
}
#endif  // TRANSLATOR_HTTP_UPSTREAM

bool TranslatorHttp::post(const std::string& url, const std::string& body, const char* contentType,
                          const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent) {
  LOG_DBG("HTTP", "POST: %s (body=%u bytes)", url.c_str(), (unsigned)body.size());
  return runPostSecure(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
}

bool TranslatorHttp::postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                              std::string& outContent) {
  const char* authName = authHeader.empty() ? nullptr : "Authorization";
  const char* authValue = authHeader.empty() ? nullptr : authHeader.c_str();
  return runPostSecure(url, jsonBody, "application/json", authName, authValue, outContent);
}

// ─── ReusableHttpSession — one kept-alive connection for a request burst ───
//
// See the class doc in TranslatorHttp.h. The reusable client only exists on the
// wolfSSL build; on the esp_http_client build (and if the client can't be
// allocated) every method delegates to the matching static, so behavior is
// identical to not using a session. The heap guard gates the FIRST request of
// the session (which performs the handshake) with the full TLS floor and every
// later, reused request with the much smaller reuse floor.

#if defined(FREEINK_NET_WOLFSSL)
struct ReusableHttpSession::Impl {
  freeink::SecureHttpClient http;
  bool everConnected = false;  // false until a request has established the connection

  Impl() {
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setInsecure();
    http.setReuse(true);  // keep the socket open between requests to the same origin
    // setUserAgent replaces the built-in UA and persists across begin() (which
    // only clears per-request headers); addHeader would append a SECOND
    // User-Agent that strict servers reject (aiohttp: 400 "Duplicate
    // 'User-Agent' header found").
    http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
    http.setFollowRedirects(MAX_REDIRECTS);  // parity with runGetWolf's manual redirect loop
  }

  // Full TLS floor on the first (handshaking) request; the cheap reuse floor after.
  bool heapRefused() const { return everConnected ? insufficientHeapForReuse() : insufficientHeapForTls(); }
};
#else
struct ReusableHttpSession::Impl {};  // no reusable client off wolfSSL; methods delegate to statics
#endif

ReusableHttpSession::ReusableHttpSession() {
#if defined(FREEINK_NET_WOLFSSL)
  impl = makeUniqueNoThrow<Impl>();
  if (!impl) {
    // Degrade gracefully rather than abort: the request methods fall back to
    // the per-request statics when impl is null.
    LOG_ERR("HTTP", "OOM: translation session client; using per-request connections");
  }
#endif
}

// unique_ptr frees Impl here; SecureHttpClient's destructor closes the socket.
ReusableHttpSession::~ReusableHttpSession() = default;

bool ReusableHttpSession::fetchUrl(const std::string& url, std::string& outContent) {
#if defined(FREEINK_NET_WOLFSSL)
  if (impl) {
    outContent.clear();
    if (impl->heapRefused()) return false;  // GET mirrors runGetWolf: does not touch lastHttpCode
    freeink::SecureHttpClient& http = impl->http;
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL(session) bad URL: %s", url.c_str());
      return false;
    }
    LOG_DBG("HTTP", "wolfSSL(session) GET: %s", url.c_str());
    const int status = http.GET();
    if (status >= 0) impl->everConnected = true;  // transport worked -> later requests take the reuse floor
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL(session) GET status %d: %s", status, url.c_str());
      return false;
    }
    if (!http.responseComplete()) {
      LOG_ERR("HTTP", "wolfSSL(session) incomplete body");
      return false;
    }
    outContent = http.getString();
    return true;
  }
#endif
  return TranslatorHttp::fetchUrl(url, outContent);
}

bool ReusableHttpSession::post(const std::string& url, const std::string& body, const char* contentType,
                               const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent) {
#if defined(FREEINK_NET_WOLFSSL)
  if (impl) {
    outContent.clear();
    if (impl->heapRefused()) {
      TranslatorHttp::lastHttpCode = -1;
      return false;
    }
    freeink::SecureHttpClient& http = impl->http;
    if (!http.begin(url)) {
      LOG_ERR("HTTP", "wolfSSL(session) bad URL: %s", url.c_str());
      TranslatorHttp::lastHttpCode = -1;
      return false;
    }
    if (contentType && *contentType) http.addHeader("Content-Type", contentType);
    if (extraHeaderName && extraHeaderValue) http.addHeader(extraHeaderName, extraHeaderValue);
    LOG_DBG("HTTP", "wolfSSL(session) POST: %s (body=%u bytes)", url.c_str(), (unsigned)body.size());
    const int status = http.sendRequest("POST", body);
    if (status >= 0) impl->everConnected = true;
    TranslatorHttp::lastHttpCode = status;
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL(session) POST failed: %s", url.c_str());
      return false;
    }
    // getString() holds the buffered body for the lifetime of `http`; copy it out.
    outContent = http.getString();
    if (status != 200) {
      LOG_ERR("HTTP", "wolfSSL(session) POST unexpected status: %d (%u byte body)", status,
              (unsigned)outContent.size());
      return false;
    }
    return true;
  }
#endif
  return TranslatorHttp::post(url, body, contentType, extraHeaderName, extraHeaderValue, outContent);
}

bool ReusableHttpSession::postJson(const std::string& url, const std::string& jsonBody, const std::string& authHeader,
                                   std::string& outContent) {
  const char* authName = authHeader.empty() ? nullptr : "Authorization";
  const char* authValue = authHeader.empty() ? nullptr : authHeader.c_str();
  return post(url, jsonBody, "application/json", authName, authValue, outContent);
}

bool ReusableHttpSession::waitForHeapReady(uint32_t timeoutMs, volatile const bool* cancelFlag) {
#if defined(FREEINK_NET_WOLFSSL)
  if (impl) {
    // Mirror heapRefused()'s floor selection: a request on an already-handshaken
    // socket pays only the small reuse floor (no handshake churn, no large
    // cert-flight record), so we must NOT wait for handshake-sized headroom a
    // kept-alive request will never need — that would needlessly stall (and
    // eventually abort) normal mid-chapter progress where free heap sits between
    // the reuse and handshake floors.
    const uint32_t minFree =
        impl->everConnected ? TranslatorHttp::MIN_FREE_HEAP_FOR_REUSE : TranslatorHttp::MIN_FREE_HEAP_FOR_TLS;
    const uint32_t minBlock = impl->everConnected ? 0u : TranslatorHttp::MIN_MAX_ALLOC_FOR_TLS;
    return heapbp::waitForHeap(minFree, minBlock, timeoutMs, cancelFlag, "HTTP",
                               impl->everConnected ? "reused TLS heap" : "TLS handshake heap");
  }
#endif
  // No heap guard applies on the esp_http_client build or when no session client
  // was allocated — proceed immediately (behavior identical to not waiting).
  (void)timeoutMs;
  (void)cancelFlag;
  return true;
}

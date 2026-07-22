#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

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

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "");

  // Last HTTP response code from the most recent post/postJson call. Negative
  // values indicate connection-level failures (bad URL, transport error, or the
  // pre-request low-heap guard); positive values are HTTP status codes. Set on
  // every exit path, including connection-level failures. Used by callers
  // (e.g. ParagraphTranslator) to surface engine-specific errors to the user.
  static int lastHttpCode;
};

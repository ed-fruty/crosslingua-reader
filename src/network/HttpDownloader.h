#pragma once
#include <HalStorage.h>

#include <functional>
#include <string>

/**
 * HTTP client utility for fetching content and downloading files.
 * Wraps WiFiClientSecure and HTTPClient for HTTPS requests.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  /**
   * Fetch text content from a URL.
   * @param url The URL to fetch
   * @param outContent The fetched content (output)
   * @return true if fetch succeeded, false on error
   */
  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

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
   * POST with configurable content type and a custom header.
   * @param url The URL to POST to
   * @param body The request body
   * @param contentType Content-Type header value (e.g. "application/json+protobuf")
   * @param extraHeaderName Extra header name (e.g. "X-Goog-Api-Key"), nullptr to skip
   * @param extraHeaderValue Extra header value
   * @param outContent The response body (output)
   * @return true if request succeeded (HTTP 200), false on error
   */
  static bool post(const std::string& url, const std::string& body, const char* contentType,
                   const char* extraHeaderName, const char* extraHeaderValue, std::string& outContent);

  /**
   * Download a file to the SD card.
   * @param url The URL to download
   * @param destPath The destination path on SD card
   * @param progress Optional progress callback
   * @return DownloadError indicating success or failure type
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

  // Last HTTP response code from fetchUrl/post/postJson (negative = connection error, e.g. -1 = DNS/TCP fail)
  static int lastHttpCode;

 private:
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 1024;
};

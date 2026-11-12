// HTTP transport SDK <-> gateway REST. Owns auth headers, error mapping,
// retries/backoff, idempotency keys, timeouts. Mirrors `@solarisdk/core` http.ts.
//
// Retry policy: idempotent requests (GET, DELETE, or any request carrying an
// Idempotency-Key) retry on network error, HTTP 5xx, and bodies flagged
// `retryable`. 429 is NOT retried (concurrency cap). Backoff: min(150*2^n,8000)+
// rand(0..250) ms, default 5 retries.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace solari {

/** A fresh idempotency key (UUID v4). */
std::string newIdempotencyKey();

/** The concrete bytes of an HTTP request — exposed so the request builder can
 *  be asserted in tests without a live server. */
struct PreparedRequest {
  std::string method;
  std::string url;
  std::vector<std::string> headers;  // "Key: Value"
  std::string body;
  bool hasBody = false;
};

/** How long a pause is retried after a retryable 503 (PauseRetryable): a pause takes 35-110 s on a loaded host. Same window as the
 *  TypeScript and Python SDKs (PAUSE_RETRY_WINDOW_MS). */
constexpr long kPauseRetryWindowMs = 120000;

class HttpTransport {
 public:
  HttpTransport(std::string apiKey, std::string baseUrl, int maxRetries = 5,
                long requestTimeoutMs = 300000, long retryDelayMs = -1);

  const std::string& baseUrl() const { return baseUrl_; }
  /** `Authorization` header value list for WS upgrades / handles. */
  std::vector<std::string> authHeaders() const;
  /** ws:// (or wss://) origin derived from the gateway base URL. */
  std::string wsOrigin() const;

  /** Build (but do not send) the request bytes. Pure — used by tests. */
  PreparedRequest prepare(const std::string& method, const std::string& path,
                          bool hasBody, const std::string& body,
                          const std::optional<std::string>& idempotencyKey) const;

  /**
   * Perform a request with the retry/backoff/error-mapping policy. `body` unset
   * means no body. Returns the parsed JSON response (null json for empty body).
   * Throws the mapped typed error on failure.
   *
   * `retryWindowMs`, when set, retries a retryable failure until that long has passed since the first attempt instead of
   * `maxRetries` times (~5 s of backoff): for calls whose server side legitimately takes a while to settle (a pause takes 35-110 s;
   * a host that cuts it short answers a retryable 503 PauseRetryable). Only an idempotent request is retried at all.
   */
  nlohmann::json request(const std::string& method, const std::string& path,
                         const std::optional<nlohmann::json>& body = std::nullopt,
                         const std::optional<std::string>& idempotencyKey =
                             std::nullopt,
                         std::optional<long> retryWindowMs = std::nullopt);

 private:
  struct RawResponse {
    long status = 0;
    std::string body;
    bool networkError = false;
    std::string errMsg;
  };
  RawResponse perform(const PreparedRequest& req);
  long backoffMs(int attempt) const;

  std::string apiKey_;
  std::string baseUrl_;
  int maxRetries_;
  long requestTimeoutMs_;
  long retryDelayMs_;  // -1 => exponential backoff + jitter
};

} // namespace solari

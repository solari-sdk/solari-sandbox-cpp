#include "solari/http.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <random>
#include <thread>

#include "solari/errors.hpp"

namespace solari {

std::string newIdempotencyKey() {
  std::random_device rd;
  std::mt19937_64 gen(((std::uint64_t)rd() << 32) ^ rd() ^
                      (std::uint64_t)std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count());
  std::uint64_t a = gen(), b = gen();
  unsigned char bytes[16];
  for (int i = 0; i < 8; i++) bytes[i] = (a >> (8 * i)) & 0xFF;
  for (int i = 0; i < 8; i++) bytes[8 + i] = (b >> (8 * i)) & 0xFF;
  bytes[6] = (bytes[6] & 0x0F) | 0x40;  // version 4
  bytes[8] = (bytes[8] & 0x3F) | 0x80;  // variant
  char buf[37];
  std::snprintf(
      buf, sizeof(buf),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6],
      bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13],
      bytes[14], bytes[15]);
  return std::string(buf);
}

static std::string stripTrailingSlashes(std::string s) {
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

HttpTransport::HttpTransport(std::string apiKey, std::string baseUrl,
                             int maxRetries, long requestTimeoutMs,
                             long retryDelayMs)
    : apiKey_(std::move(apiKey)),
      baseUrl_(stripTrailingSlashes(std::move(baseUrl))),
      maxRetries_(maxRetries),
      requestTimeoutMs_(requestTimeoutMs),
      retryDelayMs_(retryDelayMs) {
  if (apiKey_.empty()) throw Error("HttpTransport requires an apiKey");
  if (baseUrl_.empty()) throw Error("HttpTransport requires a baseUrl");
}

std::vector<std::string> HttpTransport::authHeaders() const {
  return {"Authorization: Bearer " + apiKey_};
}

std::string HttpTransport::wsOrigin() const {
  // http -> ws, https -> wss (replace leading "http").
  if (baseUrl_.rfind("http", 0) == 0) return "ws" + baseUrl_.substr(4);
  return baseUrl_;
}

PreparedRequest HttpTransport::prepare(
    const std::string& method, const std::string& path, bool hasBody,
    const std::string& body,
    const std::optional<std::string>& idempotencyKey) const {
  PreparedRequest r;
  r.method = method;
  r.url = baseUrl_ + path;
  r.hasBody = hasBody;
  r.body = body;
  r.headers.push_back("Authorization: Bearer " + apiKey_);
  r.headers.push_back("Accept: application/json");
  if (hasBody) r.headers.push_back("Content-Type: application/json");
  if (idempotencyKey) r.headers.push_back("Idempotency-Key: " + *idempotencyKey);
  return r;
}

static std::size_t writeCb(char* ptr, std::size_t size, std::size_t nmemb,
                           void* userdata) {
  auto* s = static_cast<std::string*>(userdata);
  s->append(ptr, size * nmemb);
  return size * nmemb;
}

HttpTransport::RawResponse HttpTransport::perform(const PreparedRequest& req) {
  RawResponse out;
  CURL* curl = curl_easy_init();
  if (!curl) {
    out.networkError = true;
    out.errMsg = "curl_easy_init failed";
    return out;
  }
  struct curl_slist* headers = nullptr;
  for (const auto& h : req.headers) headers = curl_slist_append(headers, h.c_str());
  // libcurl sends no Content-Length for a POST/PUT/PATCH without a body (CUSTOMREQUEST and no POSTFIELDS), and the hosted
  // gateway's front end answers 411 Length Required (pause/resume/start). Say "empty body" explicitly.
  if (!req.hasBody && (req.method == "POST" || req.method == "PUT" || req.method == "PATCH"))
    headers = curl_slist_append(headers, "Content-Length: 0");

  curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, requestTimeoutMs_);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
  if (req.hasBody) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
  }

  CURLcode rc = curl_easy_perform(curl);
  if (rc != CURLE_OK) {
    out.networkError = true;
    out.errMsg = curl_easy_strerror(rc);
  } else {
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out.status);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return out;
}

long HttpTransport::backoffMs(int attempt) const {
  if (retryDelayMs_ >= 0) return retryDelayMs_;
  long base = 150L << attempt;  // 150 * 2^attempt
  if (base > 8000) base = 8000;
  static thread_local std::mt19937 gen(std::random_device{}());
  std::uniform_int_distribution<int> jitter(0, 249);
  return base + jitter(gen);
}

nlohmann::json HttpTransport::request(
    const std::string& method, const std::string& path,
    const std::optional<nlohmann::json>& body,
    const std::optional<std::string>& idempotencyKey) {
  const bool idempotent =
      method == "GET" || method == "DELETE" || idempotencyKey.has_value();
  const bool hasBody = body.has_value();
  const std::string bodyStr = hasBody ? body->dump() : std::string();
  PreparedRequest req = prepare(method, path, hasBody, bodyStr, idempotencyKey);

  int attempt = 0;
  for (;;) {
    RawResponse res = perform(req);
    if (res.networkError) {
      if (idempotent && attempt < maxRetries_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs(attempt++)));
        continue;
      }
      throw ConnectionError(method + " " + path + " failed: " + res.errMsg);
    }

    if (res.status < 200 || res.status >= 300) {
      nlohmann::json errBody;
      try {
        if (!res.body.empty()) errBody = nlohmann::json::parse(res.body);
      } catch (...) {
        errBody = nlohmann::json();
      }
      // 🚨 ALLOWLIST, NOT >= 500 — a 5xx is not automatically transient. The
      // gateway's seven 501 sites (every /volumes route, permanently unavailable on
      // GCP) were each retried 5x with backoff; the curve is visible in prod LB logs
      // (04:47:11, +24s, +47s — doubling, which separates a retry loop from an
      // evenly-spaced poll).
      // 🪤 507 is named here rather than left to the retryable hint: the hint would
      // cover it today, but that ties this SDK's correctness to which gateway build
      // the caller reaches. 500 excluded (an unhandled error is not known-transient);
      // 429 excluded as before.
      const bool retryableStatus =
          res.status == 502 || res.status == 503 || res.status == 504 || res.status == 507;
      bool retryable =
          retryableStatus || (errBody.is_object() && errBody.contains("retryable") &&
                                errBody["retryable"].is_boolean() &&
                                errBody["retryable"].get<bool>());
      if (idempotent && retryable && attempt < maxRetries_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs(attempt++)));
        continue;
      }
      throwGatewayError(static_cast<int>(res.status), errBody);
    }

    if (res.body.empty()) return nlohmann::json();
    return nlohmann::json::parse(res.body);
  }
}

} // namespace solari

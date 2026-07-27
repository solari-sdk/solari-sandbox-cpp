// Typed error hierarchy. Mirrors `@solarisdk/core` errors.ts: gateway HTTP
// status codes and control-channel RPC failures are mapped onto these so callers
// can `catch` a specific type or the common `solari::Error` base.
#pragma once
#include <stdexcept>
#include <string>
#include <optional>
#include <nlohmann/json.hpp>

namespace solari {

/** Base class for every error thrown by the SDK. */
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& msg) : std::runtime_error(msg) {}
};

/** Any non-2xx gateway response that isn't otherwise specialized. */
class GatewayError : public Error {
 public:
  int status;
  std::optional<std::string> code;
  nlohmann::json body;

  GatewayError(int s, const std::string& msg, nlohmann::json b = nlohmann::json())
      : Error(msg), status(s), body(std::move(b)) {
    if (body.is_object() && body.contains("code") && body["code"].is_string()) {
      code = body["code"].get<std::string>();
    }
  }
};

/** HTTP 401/403 — missing/rejected API key. */
class AuthError : public GatewayError {
 public:
  explicit AuthError(const std::string& msg = "Invalid or missing API key",
                     nlohmann::json b = nlohmann::json())
      : GatewayError(401, msg, std::move(b)) {}
};

/** HTTP 402 — the plan doesn't allow this. */
class PlanError : public GatewayError {
 public:
  explicit PlanError(const std::string& msg = "This feature requires a different plan",
                     nlohmann::json b = nlohmann::json())
      : GatewayError(402, msg, std::move(b)) {}
};

/** HTTP 429 — org is at its session cap. NOT retryable. */
class ConcurrencyLimitError : public GatewayError {
 public:
  explicit ConcurrencyLimitError(const std::string& msg = "Concurrency limit exceeded",
                                 nlohmann::json b = nlohmann::json())
      : GatewayError(429, msg, std::move(b)) {}
};

/** HTTP 503 / body `no_capacity` — no host available (retryable). */
class NoCapacityError : public GatewayError {
 public:
  explicit NoCapacityError(const std::string& msg = "No host available",
                           nlohmann::json b = nlohmann::json())
      : GatewayError(503, msg, std::move(b)) {}
};

/** A control-WS RPC returned `{ok:false}`. */
class ActionError : public Error {
 public:
  std::string method;
  std::optional<std::string> code;
  ActionError(std::string m, const std::string& message,
              std::optional<std::string> c = std::nullopt)
      : Error(message), method(std::move(m)), code(std::move(c)) {}
};

/** An RPC / connect did not complete within its timeout. */
class TimeoutError : public Error {
 public:
  std::string method;
  long timeoutMs;
  TimeoutError(std::string m, long ms)
      : Error("Action \"" + m + "\" timed out after " + std::to_string(ms) + "ms"),
        method(std::move(m)),
        timeoutMs(ms) {}
};

/** The control WebSocket is not open (never connected, or dropped). */
class ConnectionError : public Error {
 public:
  explicit ConnectionError(const std::string& msg = "Control channel is not connected")
      : Error(msg) {}
};

/**
 * Map a gateway HTTP response onto the appropriate typed error and THROW it.
 * `body` may be a null json when the error had no parseable JSON body.
 */
[[noreturn]] void throwGatewayError(int status, const nlohmann::json& body);

} // namespace solari

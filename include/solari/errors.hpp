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

/** HTTP 401 — missing/rejected API key. */
class AuthError : public GatewayError {
 public:
  explicit AuthError(const std::string& msg = "Invalid or missing API key",
                     nlohmann::json b = nlohmann::json())
      : GatewayError(401, msg, std::move(b)) {}
};

/**
 * The plan doesn't allow this: HTTP 402 (FeatureRequiresPlan, InsufficientCredit) or
 * 403 {code:"NotEntitled"} (the plan does not include sandboxes/desktops). `status` is
 * the status the gateway really sent (402 unless a 403 NotEntitled built it).
 */
class PlanError : public GatewayError {
 public:
  explicit PlanError(const std::string& msg = "This feature requires a different plan",
                     nlohmann::json b = nlohmann::json(), int status = 402)
      : GatewayError(status, msg, std::move(b)) {}
};

/**
 * HTTP 429 {code:"ConcurrencyLimitExceeded"} (or a 429 with no code) — org is at its
 * session cap. NOT retryable. Other 429s (TooManyExecutions, a per-sandbox limit,
 * ObjectLimit, the template caps) are plain GatewayError with the gateway's code.
 */
class ConcurrencyLimitError : public GatewayError {
 public:
  explicit ConcurrencyLimitError(const std::string& msg = "Concurrency limit exceeded",
                                 nlohmann::json b = nlohmann::json())
      : GatewayError(429, msg, std::move(b)) {}
};

/**
 * HTTP 503 {code:"NoCapacity"|"StartTimeout"} (or a 503 with no code) / body
 * `no_capacity` — no host available (retryable). Other 503s (SandboxUnavailable,
 * StoreUnavailable, AdmissionUnavailable, DependencyUnavailable) are plain
 * GatewayError: the sandbox, store or admission service is unavailable, which is not a
 * capacity problem.
 */
class NoCapacityError : public GatewayError {
 public:
  explicit NoCapacityError(const std::string& msg = "No host available",
                           nlohmann::json b = nlohmann::json())
      : GatewayError(503, msg, std::move(b)) {}
};

/**
 * HTTP 409 {code:"NotStopped"} or {code:"StartInProgress"} from `start`. NotStopped: the sandbox is in
 * another state (use `resume()` for a paused one). StartInProgress: a start is already under way, so
 * poll until the sandbox is running. Not retryable as-is. `state` is the sandbox's current state when
 * the gateway reports it. Host refusals (StoppedDiskMissing, GoldenMismatch, HostCapabilityMissing, ...)
 * stay plain GatewayError with the host's `code`. Mirrors `SandboxNotStoppedError` in the TS and Python SDKs.
 */
class SandboxNotStoppedError : public GatewayError {
 public:
  std::optional<std::string> state;

  explicit SandboxNotStoppedError(const std::string& msg = "Sandbox is not stopped",
                                  nlohmann::json b = nlohmann::json())
      : GatewayError(409, msg, std::move(b)) {
    if (body.is_object() && body.contains("state") && body["state"].is_string()) {
      state = body["state"].get<std::string>();
    }
  }
};

/**
 * An argument the SDK refuses before any request is made (a `user` that is not `root` or a unix user name, a negative `timeoutMs`).
 * The same class the gateway's REST routes answer with: status 400, code "ValidationError". Raised by the SDK for a command on
 * every route, so a bad value fails the same way whether the command would have gone over `/exec` or the live connection.
 */
class ValidationError : public GatewayError {
 public:
  explicit ValidationError(const std::string& msg)
      : GatewayError(400, msg, nlohmann::json{{"code", "ValidationError"}, {"error", msg}}) {}
  /** The gateway's own HTTP 400 (a bad field, a limit exceeded; `code` says which), as the TypeScript, Python and Go SDKs map it. */
  ValidationError(const std::string& msg, nlohmann::json body) : GatewayError(400, msg, std::move(body)) {}
};

/**
 * HTTP 409 {code:"ExecInterrupted"} from a one-shot command (`POST /sandboxes/:id/exec`): the sandbox was parked while the command
 * ran, so it may have partly run. Never retried and never re-run on the live connection. Not retryable as-is. A GatewayError
 * (status 409, code "ExecInterrupted"), so `catch (const GatewayError&)` still catches it. The Rust SDK's
 * `SolariError::ExecInterrupted`, Go's `*ExecInterruptedError`.
 */
class ExecInterruptedError : public GatewayError {
 public:
  ExecInterruptedError(const std::string& msg, nlohmann::json body) : GatewayError(409, msg, std::move(body)) {}
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

/** A `commands.run` / `code.run` stopped because its `CancelToken` was cancelled. */
class CancelledError : public Error {
 public:
  CancelledError() : Error("The call was cancelled") {}
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

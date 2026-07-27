// `Client` — talks the SDK <-> gateway `/sandboxes` REST API and hands back
// `Sandbox` handles wired with the lifecycle hooks (exec fast path, kill).
// Ported from `@solarisdk/sandbox` sandbox-client.ts.
#pragma once
#include <memory>
#include <string>

#include "solari/http.hpp"
#include "solari/sandbox.hpp"
#include "solari/types.hpp"

namespace solari {

struct ClientOptions {
  std::string apiKey;
  std::string baseUrl;
  long callTimeoutMs = 300000;
};

/** Build the `POST /sandboxes` JSON body, omitting unset fields (public for
 *  testing the request shape). */
nlohmann::json buildCreateBody(const CreateSandboxOptions& opts,
                               SandboxKind kind = SandboxKind::Sandbox);

class Client {
 public:
  explicit Client(const ClientOptions& options);

  /** Create a new headless sandbox (`POST /sandboxes`). */
  std::shared_ptr<Sandbox> create(const CreateSandboxOptions& opts = {});
  /** Create a GUI desktop VM (`POST /sandboxes` with `kind:"desktop"`).
   *  Mirrors TS `createDesktop()` / Python `create_desktop()`. The handle
   *  carries `Sandbox::streamUrl()` for the live view and the same core
   *  surface as a sandbox; driving the GUI is not part of this surface.
   *  Requires the `desktop` entitlement (gateway 402s otherwise). */
  std::shared_ptr<Sandbox> createDesktop(const CreateSandboxOptions& opts = {});
  /** Re-attach to a running sandbox by id (`GET /sandboxes/:id`). */
  std::shared_ptr<Sandbox> connect(const std::string& sandboxId);
  /** Fetch a sandbox view (`GET /sandboxes/:id`). */
  SandboxView get(const std::string& sandboxId);
  /** Destroy a sandbox (`DELETE /sandboxes/:id`). Idempotent. */
  void kill(const std::string& sandboxId);
  /** Pause a session by id (`POST /sandboxes/:id/pause`): snapshot RAM+disk and
   *  free its host slot. Recover it with resume(). */
  void pause(const std::string& sandboxId);
  /** Resume a paused session by id (`POST /sandboxes/:id/resume`); returns the
   *  control URL to re-attach to. */
  std::string resume(const std::string& sandboxId);

  HttpTransport& http() { return *http_; }

 private:
  std::shared_ptr<Sandbox> createWithKind(const CreateSandboxOptions& opts,
                                          SandboxKind kind);
  SessionHooks makeHooks();
  SandboxConfig handleConfig();

  std::shared_ptr<HttpTransport> http_;
  std::string apiKey_;
  long callTimeoutMs_;
};

} // namespace solari

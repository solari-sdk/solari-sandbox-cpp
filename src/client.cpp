#include "solari/client.hpp"

#include "solari/errors.hpp"
#include "solari/util.hpp"

namespace solari {

nlohmann::json buildCreateBody(const CreateSandboxOptions& opts,
                               SandboxKind kind) {
  nlohmann::json body;
  body["kind"] = toString(kind);
  if (opts.template_) body["template"] = *opts.template_;
  if (opts.cpu) body["cpu"] = *opts.cpu;
  if (opts.memMb) body["memMb"] = *opts.memMb;
  if (opts.diskGb) body["diskGb"] = *opts.diskGb;
  if (!opts.envs.empty()) body["envs"] = opts.envs;
  if (!opts.metadata.empty()) body["metadata"] = opts.metadata;
  if (opts.timeoutMs) body["timeoutMs"] = *opts.timeoutMs;
  if (opts.fromSnapshot) body["fromSnapshot"] = *opts.fromSnapshot;
  if (opts.lifecycle) {
    nlohmann::json lc;
    lc["onTimeout"] = opts.lifecycle->onTimeout;
    if (opts.lifecycle->autoResume) lc["autoResume"] = *opts.lifecycle->autoResume;
    body["lifecycle"] = lc;
  }
  if (opts.resolution) body["resolution"] = *opts.resolution;
  if (opts.record) body["record"] = *opts.record;
  if (!opts.volumes.empty()) {
    nlohmann::json vols = nlohmann::json::array();
    for (const auto& v : opts.volumes) {
      vols.push_back({{"volumeId", v.volumeId}, {"path", v.path}});
    }
    body["volumes"] = vols;
  }
  return body;
}

static CommandResult parseCommandResult(const nlohmann::json& j) {
  CommandResult r;
  if (j.contains("exitCode") && j["exitCode"].is_number())
    r.exitCode = static_cast<int>(j["exitCode"].get<double>());
  if (j.contains("stdout") && j["stdout"].is_string())
    r.stdout_ = j["stdout"].get<std::string>();
  if (j.contains("stderr") && j["stderr"].is_string())
    r.stderr_ = j["stderr"].get<std::string>();
  return r;
}

Client::Client(const ClientOptions& options)
    : apiKey_(options.apiKey), callTimeoutMs_(options.callTimeoutMs) {
  if (options.apiKey.empty()) throw Error("Client requires an apiKey");
  if (options.baseUrl.empty()) throw Error("Client requires a baseUrl");
  http_ = std::make_shared<HttpTransport>(options.apiKey, options.baseUrl);
}

SessionHooks Client::makeHooks() {
  auto http = http_;
  SessionHooks hooks;
  hooks.exec = [http](const std::string& id,
                      const ExecOneShotRequest& req) -> CommandResult {
    nlohmann::json body;
    body["cmd"] = req.cmd;
    if (!req.args.empty()) body["args"] = req.args;
    if (req.cwd) body["cwd"] = *req.cwd;
    if (req.timeoutMs) body["timeoutMs"] = *req.timeoutMs;
    nlohmann::json r = http->request(
        "POST", "/sandboxes/" + encodeURIComponent(id) + "/exec", body);
    return parseCommandResult(r);
  };
  hooks.kill = [http](const std::string& id) {
    http->request("DELETE", "/sandboxes/" + encodeURIComponent(id));
  };
  hooks.pause = [http](const std::string& id) {
    http->request("POST", "/sandboxes/" + encodeURIComponent(id) + "/pause");
  };
  hooks.resume = [http](const std::string& id) -> std::string {
    nlohmann::json r =
        http->request("POST", "/sandboxes/" + encodeURIComponent(id) + "/resume");
    // The session comes back on a FRESH slot, so its pre-pause control URL is
    // stale. The gateway normally returns the new one; derive it exactly as
    // Client::connect does when it does not.
    if (r.is_object() && r.contains("controlUrl") && r["controlUrl"].is_string()) {
      return r["controlUrl"].get<std::string>();
    }
    return http->wsOrigin() + "/control/" + encodeURIComponent(id);
  };
  return hooks;
}

SandboxConfig Client::handleConfig() {
  SandboxConfig cfg;
  cfg.apiKey = apiKey_;
  cfg.callTimeoutMs = callTimeoutMs_;
  cfg.hooks = makeHooks();
  return cfg;
}

std::shared_ptr<Sandbox> Client::create(const CreateSandboxOptions& opts) {
  return createWithKind(opts, SandboxKind::Sandbox);
}

std::shared_ptr<Sandbox> Client::createDesktop(const CreateSandboxOptions& opts) {
  return createWithKind(opts, SandboxKind::Desktop);
}

std::shared_ptr<Sandbox> Client::createWithKind(const CreateSandboxOptions& opts,
                                                SandboxKind kind) {
  nlohmann::json body = buildCreateBody(opts, kind);
  nlohmann::json r =
      http_->request("POST", "/sandboxes", body, newIdempotencyKey());

  CreateSandboxResponse resp;
  resp.sandboxId = r.at("sandboxId").get<std::string>();
  if (r.contains("kind") && r["kind"].is_string()) resp.kind = r["kind"].get<std::string>();
  if (r.contains("controlUrl") && r["controlUrl"].is_string())
    resp.controlUrl = r["controlUrl"].get<std::string>();
  if (r.contains("expiresAt") && r["expiresAt"].is_string())
    resp.expiresAt = r["expiresAt"].get<std::string>();
  if (r.contains("streamUrl") && r["streamUrl"].is_string())
    resp.streamUrl = r["streamUrl"].get<std::string>();

  return std::make_shared<Sandbox>(resp, handleConfig());
}

SandboxView Client::get(const std::string& sandboxId) {
  nlohmann::json r = http_->request(
      "GET", "/sandboxes/" + encodeURIComponent(sandboxId));
  SandboxView v;
  if (r.contains("sandboxId") && r["sandboxId"].is_string())
    v.sandboxId = r["sandboxId"].get<std::string>();
  if (r.contains("kind") && r["kind"].is_string()) v.kind = r["kind"].get<std::string>();
  if (r.contains("state") && r["state"].is_string()) v.state = r["state"].get<std::string>();
  if (r.contains("expiresAt") && r["expiresAt"].is_string())
    v.expiresAt = r["expiresAt"].get<std::string>();
  if (r.contains("controlUrl") && r["controlUrl"].is_string())
    v.controlUrl = r["controlUrl"].get<std::string>();
  return v;
}

std::shared_ptr<Sandbox> Client::connect(const std::string& sandboxId) {
  SandboxView view = get(sandboxId);
  CreateSandboxResponse resp;
  resp.sandboxId = view.sandboxId;
  resp.kind = view.kind.empty() ? "sandbox" : view.kind;
  resp.expiresAt = view.expiresAt;
  // Derive controlUrl if the view didn't carry one: swap scheme + /control/<id>.
  resp.controlUrl = view.controlUrl
                        ? *view.controlUrl
                        : http_->wsOrigin() + "/control/" +
                              encodeURIComponent(sandboxId);
  // The view carries no streamUrl (the gateway's serializer omits it), so
  // derive it exactly as controlUrl is derived above — both come from the same
  // origin. Desktops only: a headless sandbox has no display to stream.
  if (resp.kind == "desktop") {
    resp.streamUrl =
        http_->wsOrigin() + "/stream/" + encodeURIComponent(sandboxId);
  }
  return std::make_shared<Sandbox>(resp, handleConfig());
}

void Client::kill(const std::string& sandboxId) {
  http_->request("DELETE", "/sandboxes/" + encodeURIComponent(sandboxId));
}

void Client::pause(const std::string& sandboxId) { makeHooks().pause(sandboxId); }

std::string Client::resume(const std::string& sandboxId) {
  return makeHooks().resume(sandboxId);
}

} // namespace solari

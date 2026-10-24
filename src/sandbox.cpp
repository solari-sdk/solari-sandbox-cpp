#include "solari/sandbox.hpp"

#include <chrono>

#include "solari/errors.hpp"
#include "solari/util.hpp"

#ifdef SOLARI_WITH_WS
#include "solari/ws_transport.hpp"
#endif

namespace solari {

// --- CommandHandle ----------------------------------------------------------

void CommandHandle::onData(
    std::function<void(const std::string&, const std::string&)> cb) {
  std::lock_guard<std::mutex> lk(st_->m);
  if (!st_->buffered.empty()) {
    for (auto& c : st_->buffered) cb(c.first, c.second);
    st_->buffered.clear();
  }
  st_->cbs.push_back(std::move(cb));
}

int CommandHandle::wait() { return st_->exitFut.get(); }

std::optional<int> CommandHandle::waitFor(long timeoutMs) {
  if (st_->exitFut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
    return std::nullopt;
  }
  return st_->exitFut.get();
}

void CommandHandle::stdin_write(const std::string& data) {
  ch_->call("cmd.stdin", {{"cmdId", cmdId_}, {"base64", base64Encode(data)}},
            callTimeoutMs_);
}

void CommandHandle::kill(int signal) {
  nlohmann::json p = {{"cmdId", cmdId_}};
  if (signal != 0) p["signal"] = signal;
  ch_->call("cmd.kill", p, callTimeoutMs_);
}

// --- Sandbox ----------------------------------------------------------------

Sandbox::Sandbox(const CreateSandboxResponse& resp, SandboxConfig config)
    : git([this](const std::string& c, const CommandOptions& o) {
        return runCommand(c, o);
      }),
      id_(resp.sandboxId),
      controlUrl_(resp.controlUrl),
      expiresAt_(resp.expiresAt),
      kind_(resp.kind),
      streamUrl_(resp.streamUrl),
      apiKey_(config.apiKey),
      callTimeoutMs_(config.callTimeoutMs),
      hooks_(std::move(config.hooks)),
      channel_(std::make_shared<ControlChannel>(config.callTimeoutMs)) {
  // A blank id is the one bad value that survives to the wire looking almost
  // right: it builds "/sandboxes//exec" (a double slash), not an obviously
  // wrong path, so it goes unnoticed while every call the handle makes is
  // malformed. Reject it where the handle is built. (Test blank, not absence.)
  if (resp.sandboxId.find_first_not_of(" \t\r\n\f\v") == std::string::npos) {
    throw Error(
        "Solari: session id must not be blank - an empty id builds a malformed "
        "/sandboxes// request.");
  }
}

Sandbox::~Sandbox() { channel_->close(); }

void Sandbox::connect() {
#ifdef SOLARI_WITH_WS
  if (channel_->connected()) return;
  auto ws = std::make_shared<WsControlTransport>(controlUrl_, apiKey_, channel_);
  ws->connect();  // blocks with retry; throws on failure
  ws_ = ws;
#else
  throw ConnectionError(
      "This build has no WS transport (SOLARI_WITH_WS=OFF); the control channel "
      "cannot be opened. Frame dispatch/REST/git still work.");
#endif
}

void Sandbox::close() {
  channel_->close();
#ifdef SOLARI_WITH_WS
  if (ws_) {
    std::static_pointer_cast<WsControlTransport>(ws_)->stop();
    ws_.reset();
  }
#endif
}

void Sandbox::kill() {
  if (hooks_.kill) hooks_.kill(id_);
  close();
}

void Sandbox::pause() {
  if (!hooks_.pause) throw Error("pause requires a client-created handle");
  hooks_.pause(id_);
  close();
}

void Sandbox::resume() {
  if (!hooks_.resume) throw Error("resume requires a client-created handle");
  controlUrl_ = hooks_.resume(id_);
  connect();
}

void Sandbox::start() {
  if (!hooks_.start) throw Error("start requires a client-created handle");
  controlUrl_ = hooks_.start(id_);
  connect();
}

CommandHandle Sandbox::startCommand(const std::string& cmd,
                                    const CommandOptions& opts) {
  return startCommandTapped(cmd, opts, nullptr);
}

CommandHandle Sandbox::startCommandTapped(
    const std::string& cmd, const CommandOptions& opts,
    std::function<void(const std::string&, const std::string&)> tap) {
  nlohmann::json params = {{"cmd", cmd}};
  if (!opts.args.empty()) params["args"] = opts.args;
  if (opts.cwd) params["cwd"] = *opts.cwd;
  if (!opts.env.empty()) params["env"] = opts.env;
  if (opts.user) params["user"] = *opts.user;

  nlohmann::json r = channel_->call("cmd.start", params, timeout());
  std::string cmdId = r.at("cmdId").get<std::string>();

  auto st = std::make_shared<CmdState>();
  st->exitFut = st->exitProm.get_future().share();

  channel_->onStreamClose(cmdId, [st](const std::exception_ptr& e) {
    std::lock_guard<std::mutex> lk(st->m);
    if (!st->settled) {
      st->settled = true;
      st->exitProm.set_exception(e);
    }
  });

  // Subscribed before the frame handlers below: registering them flushes frames that beat the
  // cmd.start reply, and a non-empty subscriber list skips the replay buffer, so an accumulator
  // attached later via onData() would lose that early output whenever a callback is set.
  if (tap) {
    std::lock_guard<std::mutex> lk(st->m);
    st->cbs.push_back(std::move(tap));
  }
  if (opts.onStdout || opts.onStderr) {
    std::lock_guard<std::mutex> lk(st->m);
    auto onOut = opts.onStdout;
    auto onErr = opts.onStderr;
    st->cbs.push_back([onOut, onErr](const std::string& stream,
                                     const std::string& data) {
      if (stream == "stdout") {
        if (onOut) onOut(data);
      } else {
        if (onErr) onErr(data);
      }
    });
  }

  ControlChannel* ch = channel_.get();
  channel_->onFrame("cmd.data", cmdId, [st](const nlohmann::json& f) {
    std::string stream =
        (f.contains("stream") && f["stream"].is_string())
            ? f["stream"].get<std::string>()
            : "stdout";
    std::string data =
        (f.contains("base64") && f["base64"].is_string())
            ? base64Decode(f["base64"].get<std::string>())
            : "";
    std::lock_guard<std::mutex> lk(st->m);
    if (st->cbs.empty()) {
      st->buffered.emplace_back(stream, data);
    } else {
      for (auto& cb : st->cbs) cb(stream, data);
    }
  });
  channel_->onFrame("cmd.exit", cmdId, [ch, cmdId, st](const nlohmann::json& f) {
    ch->offFrame("cmd.data", cmdId);
    ch->offFrame("cmd.exit", cmdId);
    ch->offStreamClose(cmdId);
    int code = 0;
    if (f.contains("exitCode") && f["exitCode"].is_number()) {
      code = static_cast<int>(f["exitCode"].get<double>());
    }
    std::lock_guard<std::mutex> lk(st->m);
    if (!st->settled) {
      st->settled = true;
      st->exitProm.set_value(code);
    }
  });

  return CommandHandle(ch, cmdId, st, callTimeoutMs_);
}

CommandResult Sandbox::runCommand(const std::string& cmd,
                                  const CommandOptions& opts) {
  // One-shot HTTP fast path: plain run-to-completion, no streaming/env/user,
  // channel not already open. Falls back to WS only when /exec is unavailable.
  if (hooks_.exec && !opts.background && !opts.onStdout && !opts.onStderr &&
      opts.env.empty() && !opts.user && !channel_->connected()) {
    try {
      ExecOneShotRequest req;
      req.cmd = cmd;
      req.args = opts.args;
      if (opts.cwd) req.cwd = opts.cwd;
      if (opts.timeoutMs) req.timeoutMs = opts.timeoutMs;
      return hooks_.exec(id_, req);
    } catch (const GatewayError& e) {
      if (!(e.status == 404 || e.status == 405 || e.status == 501)) throw;
      // else fall through to the WS path
    }
  }

  auto out = std::make_shared<std::string>();
  auto err = std::make_shared<std::string>();
  CommandHandle h = startCommandTapped(
      cmd, opts, [out, err](const std::string& stream, const std::string& data) {
        if (stream == "stdout") *out += data;
        else *err += data;
      });
  if (opts.background) return CommandResult{0, "", ""};
  // timeoutMs is a deadline for the command, as on the REST /exec path: when it elapses the command
  // is killed and the result is exit -1 with the output so far (the guest's own timeout result).
  // Without this a hung command waited here forever.
  if (opts.timeoutMs && *opts.timeoutMs > 0) {
    std::optional<int> code = h.waitFor(*opts.timeoutMs);
    if (!code) {
      try {
        h.kill(9);
      } catch (const Error&) {
      }
      return CommandResult{-1, *out, *err};
    }
    return CommandResult{*code, *out, *err};
  }
  int code = h.wait();
  return CommandResult{code, *out, *err};
}

RunCodeResult Sandbox::runCode(const std::string& code,
                               const RunCodeOptions& opts) {
  nlohmann::json params = {{"code", code}};
  if (opts.language) params["language"] = *opts.language;
  if (opts.contextId) params["contextId"] = *opts.contextId;
  nlohmann::json r = channel_->call("code.run", params, timeout());
  return flattenCodeResult(r, opts.onStdout, opts.onStderr);
}

// --- Files ------------------------------------------------------------------

static FsStat parseStat(const nlohmann::json& j) {
  FsStat s;
  if (j.contains("name") && j["name"].is_string()) s.name = j["name"].get<std::string>();
  if (j.contains("dir") && j["dir"].is_boolean()) s.dir = j["dir"].get<bool>();
  if (j.contains("size") && j["size"].is_number()) s.size = j["size"].get<std::int64_t>();
  if (j.contains("mode") && j["mode"].is_number()) s.mode = j["mode"].get<int>();
  if (j.contains("modTimeMs") && j["modTimeMs"].is_number())
    s.modTimeMs = j["modTimeMs"].get<std::int64_t>();
  return s;
}

std::string Sandbox::Files::read(const std::string& path) {
  nlohmann::json r = s_.channel_->call("fs.read", {{"path", path}}, s_.timeout());
  return base64Decode(r.at("base64").get<std::string>());
}

std::string Sandbox::Files::readText(const std::string& path) { return read(path); }

void Sandbox::Files::write(const std::string& path, const std::string& data,
                           std::optional<int> mode) {
  nlohmann::json p = {{"path", path}, {"base64", base64Encode(data)}};
  if (mode) p["mode"] = *mode;
  s_.channel_->call("fs.write", p, s_.timeout());
}

std::vector<FsEntry> Sandbox::Files::list(const std::string& path) {
  nlohmann::json r = s_.channel_->call("fs.list", {{"path", path}}, s_.timeout());
  std::vector<FsEntry> out;
  if (r.contains("entries") && r["entries"].is_array()) {
    for (const auto& e : r["entries"]) {
      FsEntry fe;
      if (e.contains("name") && e["name"].is_string()) fe.name = e["name"].get<std::string>();
      if (e.contains("dir") && e["dir"].is_boolean()) fe.dir = e["dir"].get<bool>();
      if (e.contains("size") && e["size"].is_number()) fe.size = e["size"].get<std::int64_t>();
      out.push_back(std::move(fe));
    }
  }
  return out;
}

FsStat Sandbox::Files::stat(const std::string& path) {
  nlohmann::json r = s_.channel_->call("fs.stat", {{"path", path}}, s_.timeout());
  return parseStat(r);
}

void Sandbox::Files::mkdir(const std::string& path) {
  s_.channel_->call("fs.mkdir", {{"path", path}}, s_.timeout());
}

void Sandbox::Files::remove(const std::string& path, bool recursive) {
  s_.channel_->call("fs.remove", {{"path", path}, {"recursive", recursive}},
                    s_.timeout());
}

void Sandbox::Files::rename(const std::string& from, const std::string& to) {
  s_.channel_->call("fs.rename", {{"from", from}, {"to", to}}, s_.timeout());
}

} // namespace solari

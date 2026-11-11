#include "solari/sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "solari/errors.hpp"
#include "solari/util.hpp"

#ifdef SOLARI_WITH_WS
#include "solari/ws_transport.hpp"
#endif

namespace solari {

// --- CommandHandle ----------------------------------------------------------

void CommandHandle::onData(
    std::function<void(const std::string&, const std::string&)> cb) {
  // Replay buffered output in order and register cb only once nothing is left to replay; the replay runs with no lock
  // held, so a callback may itself call onData (it used to deadlock on the state's mutex). Chunks that arrive meanwhile
  // are buffered again and drained by the next pass.
  for (;;) {
    std::vector<std::pair<std::string, std::string>> replay;
    {
      std::lock_guard<std::mutex> lk(st_->m);
      if (st_->buffered.empty()) {
        st_->cbs.push_back(std::move(cb));
        return;
      }
      replay.swap(st_->buffered);
    }
    for (auto& c : replay) cb(c.first, c.second);
  }
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

void CommandHandle::closeStdin() {
  ch_->call("cmd.stdin", {{"cmdId", cmdId_}, {"base64", ""}, {"eof", true}}, callTimeoutMs_);
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
      recordingUrl_(resp.recordingUrl),
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
  std::lock_guard<std::mutex> connectLock(connectMu_);
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
  params["args"] = opts.args;  // always present, an empty list when there are none (the same as the /exec body)
  if (opts.cwd) params["cwd"] = *opts.cwd;
  if (!opts.env.empty()) params["env"] = opts.env;
  if (opts.user) params["user"] = *opts.user;

  // A command that needs the live connection (env, user, callbacks, a cancel token, background) opens it on demand, as the REST
  // route is chosen without the caller doing anything: the transport is not something the caller has to manage.
  if (!channel_->connected()) connect();
  nlohmann::json r;
  try {
    r = channel_->call("cmd.start", params, timeout());
  } catch (const ActionError& e) {
    // The guest's refusal carries no code. A spawn failure (fork/exec ..., chdir ...) is ExecFailed, the code /exec reports for it.
    const std::string m = e.what();
    if (!e.code && (m.rfind("fork/exec ", 0) == 0 || m.rfind("chdir ", 0) == 0)) throw ActionError(e.method, m, std::string("ExecFailed"));
    throw;
  }
  std::string cmdId = r.at("cmdId").get<std::string>();

  auto st = std::make_shared<CmdState>();
  st->exitFut = st->exitProm.get_future().share();

  // Delivery thread: runs the callbacks and settles the exit in arrival order, never on the rx thread.
  std::thread([st] {
    for (;;) {
      CmdState::Event ev;
      {
        std::unique_lock<std::mutex> lk(st->qm);
        st->qcv.wait(lk, [&] { return !st->q.empty(); });
        ev = std::move(st->q.front());
        st->q.pop_front();
      }
      if (!ev.isExit) {
        std::vector<std::function<void(const std::string&, const std::string&)>> cbs;
        {
          std::lock_guard<std::mutex> lk(st->m);
          if (st->cbs.empty()) {
            st->buffered.emplace_back(ev.stream, ev.data);
            continue;
          }
          cbs = st->cbs;
        }
        for (auto& cb : cbs) cb(ev.stream, ev.data);
        continue;
      }
      std::lock_guard<std::mutex> lk(st->m);
      if (!st->settled) {
        st->settled = true;
        if (ev.err) st->exitProm.set_exception(ev.err);
        else st->exitProm.set_value(ev.code);
      }
      return;
    }
  }).detach();

  channel_->onStreamClose(cmdId, [st](const std::exception_ptr& e) {
    CmdState::Event ev;
    ev.isExit = true;
    ev.err = e;
    st->push(std::move(ev));
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
    CmdState::Event ev;
    ev.stream = (f.contains("stream") && f["stream"].is_string()) ? f["stream"].get<std::string>() : "stdout";
    ev.data = (f.contains("base64") && f["base64"].is_string()) ? base64Decode(f["base64"].get<std::string>()) : "";
    st->push(std::move(ev));
  });
  channel_->onFrame("cmd.exit", cmdId, [ch, cmdId, st](const nlohmann::json& f) {
    ch->offFrame("cmd.data", cmdId);
    ch->offFrame("cmd.exit", cmdId);
    ch->offStreamClose(cmdId);
    CmdState::Event ev;
    ev.isExit = true;
    if (f.contains("exitCode") && f["exitCode"].is_number()) {
      ev.code = static_cast<int>(f["exitCode"].get<double>());
    }
    st->push(std::move(ev));
  });

  return CommandHandle(ch, cmdId, st, callTimeoutMs_);
}

// A 404 on /exec means "route not served" (an older gateway) only when it is not a gateway answer: a missing route comes
// back as a plain-text "404 Not Found", a deleted or unknown sandbox as JSON ({"error":"Not found"}). The latter must reach
// the caller, or the control-channel fallback hides it behind "not connected".
static bool isRouteUnavailable(const GatewayError& e) {
  if (e.status == 405 || e.status == 501) return true;
  if (e.status != 404) return false;
  const nlohmann::json& b = e.body;
  const bool gatewayAnswer = b.is_object() && ((b.contains("error") && b["error"].is_string()) ||
                                               (b.contains("code") && b["code"].is_string()) ||
                                               (b.contains("message") && b["message"].is_string()));
  return !gatewayAnswer;
}

// The gateway refusing a command stream because the sandbox is on the shared isolation tier.
static bool isSharedRefusal(const ActionError& e) {
  if (e.code && *e.code == "NotAvailableOnSharedIsolation") return true;
  const std::string m = e.what();
  return m.find("NotAvailableOnSharedIsolation") != std::string::npos ||
         m.find("not available on a shared") != std::string::npos;
}

// The result of a command run on the control connection: text decoded as UTF-8 over each whole stream, plus the exact bytes.
static CommandResult makeResult(int code, const std::string& out, const std::string& err, bool timedOut = false) {
  CommandResult r;
  r.exitCode = code;
  r.stdout_ = utf8Lossy(out);
  r.stderr_ = utf8Lossy(err);
  r.timedOut = timedOut;
  r.stdoutBytes = out;
  r.stderrBytes = err;
  return r;
}

// Validated BEFORE a transport is chosen, with the gateway's own wording and class, so a bad value fails the same way whether the
// command would have gone over /exec (which answers 400 ValidationError) or the live connection (which accepts anything).
static void validateRunOptions(const CommandOptions& opts) {
  if (opts.timeoutMs && *opts.timeoutMs < 0)
    throw ValidationError("timeoutMs must be at least 1000 (one second), or 0 for no deadline");
  if (opts.user) {
    const std::string& u = *opts.user;
    bool ok = u == "root";
    if (!ok && !u.empty() && u.size() <= 32 && ((u[0] >= 'a' && u[0] <= 'z') || u[0] == '_')) {
      ok = true;
      for (size_t i = 1; i < u.size(); i++) {
        const char c = u[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) ok = false;
      }
    }
    if (!ok) throw ValidationError("user: must be a unix user name or root");
  }
}

// The one /exec refusal that proves nothing ran: the host pool lacks the exec.user capability.
static bool isExecUserCapabilityMissing(const GatewayError& e) {
  return e.status == 409 && e.code && *e.code == "HostCapabilityMissing";
}

CommandResult Sandbox::runCommand(const std::string& cmd,
                                  const CommandOptions& opts) {
  validateRunOptions(opts);
  // One-shot HTTP fast path: plain run-to-completion, no streaming/env/user. Falls back to WS only when /exec is unavailable.
  // (A cancellable run always takes the control-channel path: the one-shot route has no command id to signal.)
  const bool restEligible = hooks_.exec && !opts.background && !opts.onStdout && !opts.onStderr && opts.env.empty() &&
                            !opts.user && !opts.cancel;
  auto viaRest = [&]() {
    ExecOneShotRequest req;
    req.cmd = cmd;
    req.args = opts.args;
    if (opts.cwd) req.cwd = opts.cwd;
    if (opts.timeoutMs) req.timeoutMs = opts.timeoutMs;
    try {
      return hooks_.exec(id_, req);
    } catch (const GatewayError& e) {
      // Make an /exec answer indistinguishable from the same command on the live connection. A deadline that fired below the
      // guest (400 ExecTimeout, or the guest's vsock read timeout as 502 "... i/o timeout") is the elapsed-deadline RESULT, never
      // an error; a command that cannot start is the action error cmd.start / ExecFailed, as on the live connection.
      const std::string msg = e.what();
      if ((e.status == 400 && e.code && *e.code == "ExecTimeout") || (e.status == 502 && msg.find("i/o timeout") != std::string::npos)) {
        CommandResult r;
        r.exitCode = -1;
        r.timedOut = true;
        return r;
      }
      if (e.status == 400 && e.code && *e.code == "ExecFailed") throw ActionError("cmd.start", msg, std::string("ExecFailed"));
      throw;
    }
  };
  // A shared sandbox refuses command streams (cmd.start) but runs commands over REST, so once we know the sandbox is shared
  // the REST route is the only one that works, whether or not the control channel is open.
  if (restEligible && (!channel_->connected() || shared_.load())) {
    try {
      return viaRest();
    } catch (const GatewayError& e) {
      // Fall back to the live connection only when /exec is unavailable, or the host refused with 409 HostCapabilityMissing (nothing
      // ran, and the live connection reaches the guest directly, which honours `user`). Any other failure propagates.
      if (!isRouteUnavailable(e) && !isExecUserCapabilityMissing(e)) throw;
    }
  }

  auto out = std::make_shared<std::string>();
  auto err = std::make_shared<std::string>();
  auto tap = [out, err](const std::string& stream, const std::string& data) {
    if (stream == "stdout") *out += data;
    else *err += data;
  };
  std::optional<CommandHandle> started;
  try {
    started.emplace(startCommandTapped(cmd, opts, tap));
  } catch (const ActionError& e) {
    if (!isSharedRefusal(e)) throw;
    // The sandbox is shared: remember it, and run this command over REST if it can be (the refusal means nothing started, so
    // nothing can run twice).
    shared_ = true;
    if (restEligible) return viaRest();
    throw;
  }
  CommandHandle& h = *started;
  if (opts.background) return CommandResult{0, "", ""};
  if (opts.cancel) return waitCancellable(h, opts, out, err);
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
      return makeResult(-1, *out, *err, true);
    }
    return makeResult(*code, *out, *err);
  }
  int code = h.wait();
  return makeResult(code, *out, *err);
}

// What a cancelled run does to the guest process: SIGTERM and a closed stdin now, SIGKILL after kCancelKillGraceMs if it has
// not exited. Every call is best effort (a command that already finished answers with an error); the escalation runs on its
// own thread so the cancelled caller returns at once.
static constexpr long kCancelKillGraceMs = 2000;

static void stopAfterCancel(CommandHandle h, std::shared_ptr<ControlChannel> keepAlive) {
  try {
    h.kill(15);
  } catch (const Error&) {
  }
  try {
    h.closeStdin();
  } catch (const Error&) {
  }
  std::thread([h, keepAlive]() mutable {
    if (h.waitFor(kCancelKillGraceMs)) return;
    try {
      h.kill(9);
    } catch (const Error&) {
    }
  }).detach();
}

CommandResult Sandbox::waitCancellable(CommandHandle& h, const CommandOptions& opts,
                                       const std::shared_ptr<std::string>& out,
                                       const std::shared_ptr<std::string>& err) {
  using clock = std::chrono::steady_clock;
  const bool hasDeadline = opts.timeoutMs && *opts.timeoutMs > 0;
  const auto deadline = clock::now() + std::chrono::milliseconds(hasDeadline ? *opts.timeoutMs : 0);
  for (;;) {
    if (opts.cancel->isCancelled()) {
      stopAfterCancel(h, channel_);
      throw CancelledError();
    }
    long slice = 50;
    if (hasDeadline) {
      const long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
      if (left <= 0) {
        try {
          h.kill(9);
        } catch (const Error&) {
        }
        return makeResult(-1, *out, *err, true);
      }
      slice = std::min(slice, left);
    }
    if (std::optional<int> code = h.waitFor(slice)) return makeResult(*code, *out, *err);
  }
}

RunCodeResult Sandbox::runCode(const std::string& code,
                               const RunCodeOptions& opts) {
  nlohmann::json params = {{"code", code}};
  if (opts.language) params["language"] = *opts.language;
  if (opts.contextId) params["contextId"] = *opts.contextId;
  // Live output: with a callback the request carries stream:true and a streaming guest sends v1 output frames before the
  // result; they reach the callbacks as they arrive, and flattenCodeResult delivers only the rest of the result's text.
  std::shared_ptr<CodeStream> stream;
  RpcStreamHandler onStream;
  if (opts.onStdout || opts.onStderr) {
    params["stream"] = true;
    stream = std::make_shared<CodeStream>(opts.onStdout, opts.onStderr);
    onStream = [stream](const nlohmann::json& f) { stream->onFrame(f); };
  }
  if (!opts.cancel) {
    nlohmann::json r = channel_->call("code.run", params, timeout(), "", onStream);
    return flattenCodeResult(r, opts.onStdout, opts.onStderr, stream ? stream->stdoutSent() : std::string(),
                             stream ? stream->stderrSent() : std::string());
  }
  // Cancellable: the call blocks, so it runs on its own thread while this one watches the token.
  struct Outcome {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    nlohmann::json result;
    std::exception_ptr error;
  };
  auto outcome = std::make_shared<Outcome>();
  auto channel = channel_;
  const long callTimeout = timeout();
  std::thread([outcome, channel, params, callTimeout, onStream] {
    nlohmann::json r;
    std::exception_ptr e;
    try {
      r = channel->call("code.run", params, callTimeout, "", onStream);
    } catch (...) {
      e = std::current_exception();
    }
    std::lock_guard<std::mutex> lk(outcome->m);
    outcome->result = std::move(r);
    outcome->error = e;
    outcome->done = true;
    outcome->cv.notify_all();
  }).detach();
  std::unique_lock<std::mutex> lk(outcome->m);
  while (!outcome->done) {
    if (opts.cancel->isCancelled()) {
      lk.unlock();
      // Ask the guest to stop the code (best effort: a guest that predates code.interrupt answers "unknown method").
      std::thread([channel, ctx = opts.contextId, callTimeout] {
        nlohmann::json p = nlohmann::json::object();
        if (ctx) p["contextId"] = *ctx;
        try {
          channel->call("code.interrupt", p, callTimeout);
        } catch (...) {
        }
      }).detach();
      throw CancelledError();
    }
    outcome->cv.wait_for(lk, std::chrono::milliseconds(50));
  }
  if (outcome->error) std::rethrow_exception(outcome->error);
  return flattenCodeResult(outcome->result, opts.onStdout, opts.onStderr, stream ? stream->stdoutSent() : std::string(),
                           stream ? stream->stderrSent() : std::string());
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

namespace {
// How much one fs.upload / fs.download call moves. A bigger frame (a 48 MiB file is a 64 MiB frame once base64'd) closes the
// control channel and fails every call in flight on it.
constexpr std::size_t kFileChunkSize = 4u << 20;
// The most write() sends in one frame; upload() has no such limit.
constexpr std::size_t kMaxSingleFrameBytes = 32u << 20;

bool isUnknownMethod(const ActionError& e) {
  std::string m = std::string(e.what()) + " " + e.code.value_or("");
  for (auto& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return m.find("unknown method") != std::string::npos || m.find("method not found") != std::string::npos ||
         m.find("unknown_method") != std::string::npos;
}
}  // namespace

std::string Sandbox::Files::read(const std::string& path) {
  // Chunked, so a large file never travels as one frame; a guest image that predates fs.download is read in one frame.
  try {
    return download(path);
  } catch (const ActionError& e) {
    if (!isUnknownMethod(e)) throw;
  }
  nlohmann::json r = s_.channel_->call("fs.read", {{"path", path}}, s_.timeout());
  return base64Decode(r.at("base64").get<std::string>());
}

std::string Sandbox::Files::download(const std::string& path) {
  std::string out;
  std::int64_t offset = 0;
  for (;;) {
    nlohmann::json r = s_.channel_->call(
        "fs.download", {{"path", path}, {"offset", offset}, {"length", kFileChunkSize}}, s_.timeout());
    std::string chunk = base64Decode(r.value("base64", std::string()));
    out += chunk;
    if (r.value("eof", false)) return out;
    std::int64_t next = offset + static_cast<std::int64_t>(chunk.size());
    if (r.contains("nextOffset") && r["nextOffset"].is_number()) next = r["nextOffset"].get<std::int64_t>();
    if (next <= offset) throw Error("Solari: fs.download made no progress at offset " + std::to_string(offset));
    offset = next;
  }
}

void Sandbox::Files::upload(const std::string& path, const std::string& data) {
  std::size_t offset = 0;
  for (;;) {
    const std::size_t end = std::min(offset + kFileChunkSize, data.size());
    s_.channel_->call("fs.upload",
                      {{"path", path},
                       {"base64", base64Encode(data.substr(offset, end - offset))},
                       {"offset", offset},
                       {"truncate", offset == 0}},
                      s_.timeout());
    if (end >= data.size()) return;
    offset = end;
  }
}

std::string Sandbox::Files::readText(const std::string& path) { return read(path); }

void Sandbox::Files::write(const std::string& path, const std::string& data,
                           std::optional<int> mode) {
  if (data.size() > kMaxSingleFrameBytes) {
    throw Error("file too large for a single call (" + std::to_string(data.size()) + " bytes, max " +
                std::to_string(kMaxSingleFrameBytes) +
                "): use files.upload()/files.download() or uploadUrl()");
  }
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

#include "solari/control_channel.hpp"

#include <chrono>
#include <thread>
#include <utility>

#include "solari/errors.hpp"
#include "solari/util.hpp"

namespace solari {

static const char US = '\x1f';  // unit separator, used to key (type,streamId)

ControlChannel::ControlChannel(long callTimeoutMs)
    : callTimeoutMs_(callTimeoutMs) {}

ControlChannel::~ControlChannel() {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    idleStop_ = true;
    idleCv_.notify_all();
  }
  if (idleThread_.joinable()) {
    if (idleThread_.get_id() == std::this_thread::get_id()) idleThread_.detach();
    else idleThread_.join();
  }
}

void ControlChannel::setIdleCloseMs(long ms) {
  std::lock_guard<std::mutex> lk(mtx_);
  idleCloseMs_ = ms;
  armIdleLocked();
}
void ControlChannel::setIdleCloseHook(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(mtx_);
  idleCloseHook_ = std::move(fn);
}
void ControlChannel::setReconnectHook(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(mtx_);
  reconnectHook_ = std::move(fn);
}

// Nothing needs the socket: no call in flight, no command close hook, no frame handler (running command, PTY, watch).
bool ControlChannel::inUseLocked() const {
  if (!pending_.empty() || !streamClosers_.empty()) return true;
  for (const auto& kv : frameHandlers_)
    if (!kv.second.empty()) return true;
  return false;
}

void ControlChannel::armIdle() {
  std::lock_guard<std::mutex> lk(mtx_);
  armIdleLocked();
}

void ControlChannel::armIdleLocked() {
  if (idleCloseMs_ <= 0 || !connected_ || closed_ || inUseLocked()) {
    idleArmed_ = false;
    idleCv_.notify_all();
    return;
  }
  idleDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(idleCloseMs_);
  idleArmed_ = true;
  if (!idleThread_.joinable()) idleThread_ = std::thread([this] { idleLoop(); });
  idleCv_.notify_all();
}

void ControlChannel::idleLoop() {
  std::unique_lock<std::mutex> lk(mtx_);
  while (!idleStop_) {
    if (!idleArmed_) {
      idleCv_.wait(lk);
      continue;
    }
    if (idleCv_.wait_until(lk, idleDeadline_) == std::cv_status::no_timeout) continue;  // re-armed, disarmed or stopped
    if (idleStop_ || !idleArmed_ || std::chrono::steady_clock::now() < idleDeadline_) continue;
    idleArmed_ = false;
    if (!connected_ || closed_ || inUseLocked()) continue;
    // Mark the channel not connected BEFORE the socket is closed: the socket's close is then not read as a drop.
    connected_ = false;
    idleClosed_ = true;
    auto hook = idleCloseHook_;
    lk.unlock();
    if (hook) hook();
    lk.lock();
  }
}

void ControlChannel::setSend(SendFn fn) {
  std::lock_guard<std::mutex> lk(mtx_);
  send_ = std::move(fn);
}

void ControlChannel::setConnected(bool v) {
  std::lock_guard<std::mutex> lk(mtx_);
  connected_ = v;
  if (v) {
    closed_ = false;
    idleClosed_ = false;
  }
  armIdleLocked();
}

bool ControlChannel::connected() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return connected_;
}

std::string ControlChannel::newId() {
  std::lock_guard<std::mutex> lk(mtx_);
  return std::to_string(nextId_++);
}

void ControlChannel::onMessage(const std::string& chunk) {
  // Accumulate + split on '\n'. Everything here runs on the socket rx thread.
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    rxBuffer_ += chunk;
    std::size_t nl;
    while ((nl = rxBuffer_.find('\n')) != std::string::npos) {
      std::string line = rxBuffer_.substr(0, nl);
      rxBuffer_.erase(0, nl + 1);
      std::string trimmed = trim(line);
      if (!trimmed.empty()) lines.push_back(std::move(trimmed));
    }
  }
  for (auto& l : lines) dispatch(l);
}

void ControlChannel::dispatch(const std::string& line) {
  nlohmann::json frame;
  try {
    frame = nlohmann::json::parse(line);
  } catch (...) {
    return;  // ignore non-JSON noise
  }
  if (!frame.is_object()) return;

  // (3) v2 async STREAM frame: dispatched by `type` + its own stream id.
  if (frame.contains("type") && frame["type"].is_string()) {
    std::string type = frame["type"].get<std::string>();
    std::string streamId;
    for (const char* k : {"cmdId", "ptyId", "watchId"}) {
      if (frame.contains(k) && frame[k].is_string()) {
        streamId = frame[k].get<std::string>();
        break;
      }
    }
    AsyncFrameHandler handler;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = frameHandlers_.find(type);
      if (it != frameHandlers_.end()) {
        auto jt = it->second.find(streamId);
        if (jt != it->second.end()) handler = jt->second;
      }
      if (!handler) {
        // Handler not registered yet — stash for flush on onFrame().
        if (orphanCount_ < kMaxOrphanFrames) {
          orphanFrames_[type + US + streamId].push_back(frame);
          orphanCount_++;
        }
      }
    }
    if (handler) handler(frame);  // invoke outside the lock (may re-enter)
    return;
  }

  if (!frame.contains("id") || !frame["id"].is_string()) return;
  std::string id = frame["id"].get<std::string>();

  // (2) v1 streamed-exec frame: route to the per-id stream handler; the call
  // stays pending for its terminal {ok,...} reply.
  if (frame.contains("stream") && frame["stream"].is_string()) {
    RpcStreamHandler h;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = streamHandlers_.find(id);
      if (it != streamHandlers_.end()) h = it->second;
    }
    if (h) h(frame);
    return;
  }

  // (1) JSON-RPC reply.
  Pending p;
  bool found = false;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
      p = it->second;
      pending_.erase(it);
      streamHandlers_.erase(id);
      found = true;
    }
  }
  if (!found) return;

  bool ok = frame.contains("ok") && frame["ok"].is_boolean() &&
            frame["ok"].get<bool>();
  if (ok) {
    p.prom->set_value(frame.contains("result") ? frame["result"] : nlohmann::json());
  } else {
    // normalizeRpcError: string error -> message; else message ?? code.
    std::string message = "Action failed";
    std::optional<std::string> code;
    if (frame.contains("error")) {
      const auto& e = frame["error"];
      if (e.is_string()) {
        message = e.get<std::string>();
      } else if (e.is_object()) {
        if (e.contains("message") && e["message"].is_string()) {
          message = e["message"].get<std::string>();
        } else if (e.contains("code") && e["code"].is_string()) {
          message = e["code"].get<std::string>();
        }
        if (e.contains("code") && e["code"].is_string()) {
          code = e["code"].get<std::string>();
        }
      }
    }
    p.prom->set_exception(
        std::make_exception_ptr(ActionError(p.method, message, code)));
  }
}

nlohmann::json ControlChannel::call(const std::string& method,
                                    const nlohmann::json& params, long timeoutMs,
                                    const std::string& presetId,
                                    RpcStreamHandler onStream) {
  // The channel may have become idle once this call has returned (or failed).
  struct Rearm {
    ControlChannel* c;
    ~Rearm() { c->armIdle(); }
  } rearm{this};
  std::function<void()> reopen;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (idleClosed_ && !closed_ && !connected_) reopen = reconnectHook_;
  }
  if (reopen) reopen();  // closed for being idle, not by close() or a drop: reopen it by itself
  return callImpl(method, params, timeoutMs, presetId, std::move(onStream));
}

nlohmann::json ControlChannel::callImpl(const std::string& method, const nlohmann::json& params, long timeoutMs,
                                        const std::string& presetId, RpcStreamHandler onStream) {
  long tmo = timeoutMs > 0 ? timeoutMs : callTimeoutMs_;
  auto prom = std::make_shared<std::promise<nlohmann::json>>();
  auto fut = prom->get_future();
  std::string id;
  SendFn send;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    if (closed_) throw ConnectionError("Control channel is closed");
    if (!connected_) throw ConnectionError("Not connected — call connect() first");
    id = presetId.empty() ? std::to_string(nextId_++) : presetId;
    pending_[id] = Pending{prom, method};
    if (onStream) streamHandlers_[id] = std::move(onStream);
    send = send_;
  }

  nlohmann::json req = {{"id", id}, {"method", method}, {"params", params}};
  std::string wire = req.dump() + "\n";
  try {
    if (send) send(wire);
    else throw ConnectionError("No transport attached to control channel");
  } catch (...) {
    std::lock_guard<std::mutex> lk(mtx_);
    pending_.erase(id);
    streamHandlers_.erase(id);
    throw;
  }

  if (fut.wait_for(std::chrono::milliseconds(tmo)) ==
      std::future_status::timeout) {
    {
      std::lock_guard<std::mutex> lk(mtx_);
      pending_.erase(id);
      streamHandlers_.erase(id);
    }
    throw TimeoutError(method, tmo);
  }
  return fut.get();  // rethrows ActionError / ConnectionError on failure
}

void ControlChannel::onFrame(const std::string& type, const std::string& id,
                             AsyncFrameHandler handler) {
  // Replay frames that arrived before this handler existed (e.g. output packed
  // into the same websocket message as the cmd.start reply) in arrival order,
  // and register the handler only once nothing is left to replay. Registering
  // first would let the rx thread deliver a newer live frame while an older
  // parked one is still being replayed here, reordering the stream. Frames that
  // arrive during a replay are parked again and drained by the next pass, so
  // each is delivered exactly once and in order.
  const auto key = type + US + id;
  for (;;) {
    std::vector<nlohmann::json> flush;
    {
      std::lock_guard<std::mutex> lk(mtx_);
      auto it = orphanFrames_.find(key);
      if (it == orphanFrames_.end()) {
        frameHandlers_[type][id] = handler;
        return;
      }
      flush = std::move(it->second);
      orphanFrames_.erase(it);
      orphanCount_ -= flush.size();
    }
    for (auto& f : flush) handler(f);  // outside the lock (may re-enter)
  }
}

void ControlChannel::offFrame(const std::string& type, const std::string& id) {
  std::lock_guard<std::mutex> lk(mtx_);
  auto it = frameHandlers_.find(type);
  if (it != frameHandlers_.end()) it->second.erase(id);
  armIdleLocked();
}

void ControlChannel::onStreamClose(const std::string& id, StreamCloser cb) {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    // If the channel is still live, register normally; failAll() will invoke
    // the closer on close. This check + store is atomic w.r.t. failAll()'s swap.
    if (connected_ && !closed_) {
      streamClosers_[id] = std::move(cb);
      return;
    }
  }
  // The channel already went down between call() returning and this
  // registration (a real thread race); fire the closer now so the frame-based
  // exit-wait can't hang forever. Invoke outside the lock.
  cb(std::make_exception_ptr(ConnectionError("Control channel is closed")));
}

void ControlChannel::offStreamClose(const std::string& id) {
  std::lock_guard<std::mutex> lk(mtx_);
  streamClosers_.erase(id);
  armIdleLocked();
}

void ControlChannel::failAll(const std::string& message) {
  std::map<std::string, Pending> pend;
  std::map<std::string, StreamCloser> closers;
  {
    std::lock_guard<std::mutex> lk(mtx_);
    pend.swap(pending_);
    closers.swap(streamClosers_);
    streamHandlers_.clear();
    frameHandlers_.clear();
    orphanFrames_.clear();
    orphanCount_ = 0;
  }
  auto err = std::make_exception_ptr(ConnectionError(message));
  for (auto& kv : pend) {
    try {
      kv.second.prom->set_exception(err);
    } catch (...) {
      // promise already satisfied — ignore
    }
  }
  for (auto& kv : closers) kv.second(err);
}

std::size_t ControlChannel::streamCloserCount() const {
  std::lock_guard<std::mutex> lk(mtx_);
  return streamClosers_.size();
}

void ControlChannel::onClose(int code, const std::string& reason) {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    connected_ = false;
  }
  std::string msg = "Control channel closed (" + std::to_string(code);
  if (!reason.empty()) msg += ": " + reason;
  msg += ")";
  failAll(msg);
}

void ControlChannel::close() {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    closed_ = true;
    connected_ = false;
    idleClosed_ = false;
    idleArmed_ = false;
    idleCv_.notify_all();
  }
  failAll("Control channel closed");
}

} // namespace solari

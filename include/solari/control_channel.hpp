// `ControlChannel` — the newline-delimited-JSON control-WS transport, ported
// 1:1 from `@solarisdk/core` transport.ts.
//
// THREE frame kinds share the socket and are dispatched BY SHAPE:
//   (1) JSON-RPC reply       {id, ok, result?|error?}  -> resolves call `id`.
//   (2) v1 streamed exec      {id, stream, data}         -> per-id stream handler.
//   (3) v2 async STREAM frame {type, cmdId|ptyId|watchId, ...} -> per (type,id)
//       handler, with orphan-frame buffering + flush on registration.
//
// The channel is transport-agnostic: it is fed raw bytes via onMessage() and
// emits frames via an injected send function. This lets the frame logic be
// unit-tested with no socket (feed byte chunks directly). The IXWebSocket wiring
// lives in ws_transport.cpp and simply plugs setSend()/onMessage()/onClose().
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>

namespace solari {

/** A v2 async STREAM frame delivered to a per-`type` handler. */
using AsyncFrameHandler = std::function<void(const nlohmann::json&)>;
/** A v1 streamed-exec chunk `{stream, data?}` delivered to a per-id handler. */
using RpcStreamHandler = std::function<void(const nlohmann::json&)>;
/** Called when the channel closes while a stream (cmd/pty) is in flight. */
using StreamCloser = std::function<void(const std::exception_ptr&)>;

class ControlChannel {
 public:
  using SendFn = std::function<void(const std::string&)>;

  explicit ControlChannel(long callTimeoutMs = 300000);
  ~ControlChannel();

  // --- idle close -----------------------------------------------------------
  // An open control socket counts as activity at the gateway (it pushes the idle deadline out every 60 s), so a socket left
  // open keeps an idle sandbox from pausing or expiring on time. With nothing using the channel (no call in flight, no
  // running command, no open PTY or watch) for `ms`, the channel calls the idle-close hook (which closes the socket quietly)
  // and the next call() reopens it through the reconnect hook. 0 or less disables it. Default 3000, as in the TS and Python SDKs.
  void setIdleCloseMs(long ms);
  /** Closes the socket WITHOUT reporting a drop (the channel has already marked itself not connected). */
  void setIdleCloseHook(std::function<void()> fn);
  /** Reopens the socket after an idle close; called by call() on the calling thread. */
  void setReconnectHook(std::function<void()> fn);

  // --- transport wiring (set by the WS adapter or tests) --------------------
  void setSend(SendFn fn);
  void setConnected(bool v);
  bool connected() const;

  /** Feed raw bytes off the socket. Splits on '\n' and dispatches each line. */
  void onMessage(const std::string& chunk);
  /** The socket closed: fail all in-flight calls + stream waits, drop state. */
  void onClose(int code, const std::string& reason);
  /** Close locally (rejects in-flight work with a ConnectionError). */
  void close();

  // --- RPC ------------------------------------------------------------------
  /** Allocate the next opaque string request id (per-channel counter from "1"). */
  std::string newId();

  /**
   * Send one JSON-RPC call and block until its correlated reply (or timeout).
   * `presetId` lets callers pre-allocate the id (so they can subscribe to v2
   * async frames keyed on the same id before the reply lands). `onStream`
   * receives v1 `{id,stream,data}` frames until the terminal reply.
   * Returns the `result` json (may be null). Throws on ok:false / timeout /
   * channel close.
   */
  nlohmann::json call(const std::string& method, const nlohmann::json& params,
                      long timeoutMs = -1, const std::string& presetId = "",
                      RpcStreamHandler onStream = nullptr);

  // --- v2 async frame subscriptions -----------------------------------------
  /** Register a handler for (type,id); flushes any orphan frames buffered
   *  before this registration. */
  void onFrame(const std::string& type, const std::string& id,
               AsyncFrameHandler handler);
  void offFrame(const std::string& type, const std::string& id);

  // --- stream close callbacks (frame-based waits, e.g. commands.run) --------
  void onStreamClose(const std::string& id, StreamCloser cb);
  /** How many stream-close hooks are registered (one per live command/PTY/watch handle). A hook that outlives its stream makes the
   *  channel count the socket as in use; exposed so a test can assert none leaks. */
  std::size_t streamCloserCount() const;
  void offStreamClose(const std::string& id);

 private:
  struct Pending {
    std::shared_ptr<std::promise<nlohmann::json>> prom;
    std::string method;
  };

  nlohmann::json callImpl(const std::string& method, const nlohmann::json& params, long timeoutMs, const std::string& presetId,
                          RpcStreamHandler onStream);
  void dispatch(const std::string& line);
  void failAll(const std::string& message);
  bool inUseLocked() const;
  void armIdle();  // takes mtx_
  void armIdleLocked();
  void idleLoop();

  mutable std::mutex mtx_;
  SendFn send_;
  bool connected_ = false;
  bool closed_ = false;
  long callTimeoutMs_;

  std::string rxBuffer_;
  std::uint64_t nextId_ = 1;

  std::map<std::string, Pending> pending_;
  std::map<std::string, RpcStreamHandler> streamHandlers_;
  // type -> (streamId -> handler)
  std::map<std::string, std::map<std::string, AsyncFrameHandler>> frameHandlers_;
  // "type\x1fstreamId" -> buffered frames
  std::map<std::string, std::vector<nlohmann::json>> orphanFrames_;
  std::size_t orphanCount_ = 0;
  static constexpr std::size_t kMaxOrphanFrames = 1024;
  std::map<std::string, StreamCloser> streamClosers_;

  // idle close
  long idleCloseMs_ = 3000;
  bool idleClosed_ = false;  // closed by the idle timer, not by close() or a drop: call() reopens it
  bool idleArmed_ = false;
  bool idleStop_ = false;
  std::chrono::steady_clock::time_point idleDeadline_;
  std::condition_variable idleCv_;
  std::thread idleThread_;
  std::function<void()> idleCloseHook_;
  std::function<void()> reconnectHook_;
};

} // namespace solari

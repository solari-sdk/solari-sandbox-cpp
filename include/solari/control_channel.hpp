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
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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
  void offStreamClose(const std::string& id);

 private:
  struct Pending {
    std::shared_ptr<std::promise<nlohmann::json>> prom;
    std::string method;
  };

  void dispatch(const std::string& line);
  void failAll(const std::string& message);

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
};

} // namespace solari

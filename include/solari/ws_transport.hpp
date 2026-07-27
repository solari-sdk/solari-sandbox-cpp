// IXWebSocket adapter for the control channel. This is the ONLY file that knows
// about IXWebSocket; the frame logic in ControlChannel is transport-agnostic and
// fully unit-tested without a socket. Compiled only when SOLARI_WITH_WS=ON.
#pragma once
#ifdef SOLARI_WITH_WS
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include <ixwebsocket/IXWebSocket.h>

#include "solari/control_channel.hpp"

namespace solari {

class WsControlTransport {
 public:
  WsControlTransport(std::string url, std::string apiKey,
                     std::shared_ptr<ControlChannel> channel);
  ~WsControlTransport();

  /** Open the WS with connect retry (3 fast-fail attempts, 150*attempt backoff,
   *  15s connect timeout, no retry on a full timeout). Throws on failure. */
  void connect();
  void stop();

 private:
  enum class Attempt { Ok, FastFail, Timeout };
  Attempt connectOnce(std::string& errOut);

  std::string url_;
  std::string apiKey_;
  std::shared_ptr<ControlChannel> channel_;
  std::unique_ptr<ix::WebSocket> ws_;

  std::mutex m_;
  std::condition_variable cv_;
  bool opened_ = false;
  bool failed_ = false;
  std::string errMsg_;
};

} // namespace solari
#endif // SOLARI_WITH_WS

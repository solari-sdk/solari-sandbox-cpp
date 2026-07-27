#include "solari/ws_transport.hpp"
#ifdef SOLARI_WITH_WS

#include <chrono>
#include <thread>

#include "solari/errors.hpp"

namespace solari {

static constexpr int kConnectTimeoutMs = 15000;
static constexpr int kConnectMaxAttempts = 3;
static constexpr int kConnectRetryBaseMs = 150;

WsControlTransport::WsControlTransport(std::string url, std::string apiKey,
                                       std::shared_ptr<ControlChannel> channel)
    : url_(std::move(url)),
      apiKey_(std::move(apiKey)),
      channel_(std::move(channel)) {}

WsControlTransport::~WsControlTransport() { stop(); }

WsControlTransport::Attempt WsControlTransport::connectOnce(std::string& errOut) {
  {
    std::lock_guard<std::mutex> lk(m_);
    opened_ = false;
    failed_ = false;
    errMsg_.clear();
  }
  ws_ = std::make_unique<ix::WebSocket>();
  ws_->setUrl(url_);
  ix::WebSocketHttpHeaders headers;
  headers["Authorization"] = "Bearer " + apiKey_;
  ws_->setExtraHeaders(headers);
  ws_->disableAutomaticReconnection();

  auto* chan = channel_.get();
  ws_->setOnMessageCallback([this, chan](const ix::WebSocketMessagePtr& msg) {
    switch (msg->type) {
      case ix::WebSocketMessageType::Open: {
        std::lock_guard<std::mutex> lk(m_);
        opened_ = true;
        cv_.notify_all();
        break;
      }
      case ix::WebSocketMessageType::Message:
        chan->onMessage(msg->str);
        break;
      case ix::WebSocketMessageType::Error: {
        std::lock_guard<std::mutex> lk(m_);
        if (!opened_) {
          failed_ = true;
          errMsg_ = msg->errorInfo.reason;
          cv_.notify_all();
        }
        break;
      }
      case ix::WebSocketMessageType::Close: {
        bool wasOpen;
        {
          std::lock_guard<std::mutex> lk(m_);
          wasOpen = opened_;
          if (!opened_) {
            failed_ = true;
            errMsg_ = msg->closeInfo.reason;
            cv_.notify_all();
          }
        }
        if (wasOpen) chan->onClose(msg->closeInfo.code, msg->closeInfo.reason);
        break;
      }
      default:
        break;
    }
  });

  ws_->start();

  std::unique_lock<std::mutex> lk(m_);
  bool signalled = cv_.wait_for(lk, std::chrono::milliseconds(kConnectTimeoutMs),
                                [this] { return opened_ || failed_; });
  if (opened_) {
    lk.unlock();
    ix::WebSocket* raw = ws_.get();
    channel_->setSend([raw](const std::string& s) { raw->sendText(s); });
    channel_->setConnected(true);
    return Attempt::Ok;
  }
  if (!signalled) {
    lk.unlock();
    ws_->stop();
    return Attempt::Timeout;
  }
  errOut = errMsg_;
  lk.unlock();
  ws_->stop();
  return Attempt::FastFail;
}

void WsControlTransport::connect() {
  std::string lastErr = "connect failed";
  for (int attempt = 1; attempt <= kConnectMaxAttempts; attempt++) {
    std::string err;
    Attempt a = connectOnce(err);
    if (a == Attempt::Ok) return;
    if (a == Attempt::Timeout) throw TimeoutError("connect", kConnectTimeoutMs);
    lastErr = err.empty() ? "connect failed" : err;
    if (attempt < kConnectMaxAttempts) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(kConnectRetryBaseMs * attempt));
    }
  }
  throw ConnectionError(lastErr);
}

void WsControlTransport::stop() {
  if (ws_) {
    ws_->stop();
    ws_.reset();
  }
}

} // namespace solari
#endif // SOLARI_WITH_WS

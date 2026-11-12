// A POST with no JSON body (pause, resume, start) must still carry `Content-Length: 0`. The hosted gateway's front end
// answers 411 Length Required to a body-less POST without it (the Rust SDK hit exactly this against the real gateway;
// libcurl with CUSTOMREQUEST and no POSTFIELDS sends none either). Loopback only.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;

namespace {

/** Answers every request with one canned 200 and records the whole request head, lower-cased. */
class HeadRecorder {
 public:
  explicit HeadRecorder(std::string body, std::size_t failFirst = 0) : body_(std::move(body)), failFirst_(failFirst) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(fd_, 8) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { loop(); });
  }
  ~HeadRecorder() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
  }
  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<std::string> heads() {
    std::lock_guard<std::mutex> lk(m_);
    return heads_;
  }

 private:
  void loop() {
    while (!stop_.load()) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 100) <= 0) continue;
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      char tmp[4096];
      ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
      if (n > 0) {
        std::string head(tmp, static_cast<std::size_t>(n));
        std::transform(head.begin(), head.end(), head.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        {
          std::lock_guard<std::mutex> lk(m_);
          heads_.push_back(head);
        }
        std::size_t seen;
        {
          std::lock_guard<std::mutex> lk(m_);
          seen = heads_.size();
        }
        // The first `failFirst_` requests get a retryable 503 PauseRetryable (the host cut a pause short).
        const std::string retryable =
            R"({"error":"Sandbox pause did not finish; it is still running","code":"PauseRetryable","retryable":true})";
        const bool fail = seen <= failFirst_;
        const std::string& payload = fail ? retryable : body_;
        const std::string resp = std::string(fail ? "HTTP/1.1 503 Service Unavailable" : "HTTP/1.1 200 OK") +
                                 "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) +
                                 "\r\nConnection: close\r\n\r\n" + payload;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
      }
      ::close(c);
    }
  }

  std::string body_;
  std::size_t failFirst_ = 0;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex m_;
  std::vector<std::string> heads_;
};

bool hasZeroLength(const std::string& head) {
  return head.find("\r\ncontent-length: 0\r\n") != std::string::npos;
}

Client clientFor(const HeadRecorder& g) {
  ClientOptions o;
  o.apiKey = "slr_test_x";
  o.baseUrl = g.baseUrl();
  return Client(o);
}

}  // namespace

TEST_CASE("pause sends Content-Length: 0 on its body-less POST") {
  HeadRecorder g(R"({"ok":true})");
  Client c = clientFor(g);
  c.pause("sbx-1");
  REQUIRE(g.heads().size() == 1);
  CHECK(g.heads()[0].rfind("post /sandboxes/sbx-1/pause ", 0) == 0);
  CHECK_MESSAGE(hasZeroLength(g.heads()[0]), "head was:\n" << g.heads()[0]);
}

TEST_CASE("resume and start send Content-Length: 0 on their body-less POSTs") {
  HeadRecorder g(R"({"state":"running","controlUrl":"wss://gw.test/control/x"})");
  Client c = clientFor(g);
  c.resume("sbx-1");
  c.start("sbx-1");
  REQUIRE(g.heads().size() == 2);
  for (const auto& h : g.heads()) CHECK_MESSAGE(hasZeroLength(h), "head was:\n" << h);
}

TEST_CASE("a GET carries no Content-Length") {
  HeadRecorder g(R"({"sandboxId":"sbx-1","kind":"sandbox","state":"running","expiresAt":"x"})");
  Client c = clientFor(g);
  c.get("sbx-1");
  REQUIRE(g.heads().size() == 1);
  CHECK(g.heads()[0].rfind("get /sandboxes/sbx-1 ", 0) == 0);
  CHECK_MESSAGE(g.heads()[0].find("content-length") == std::string::npos, "head was:\n" << g.heads()[0]);
}

TEST_CASE("pause retries a 503 PauseRetryable with one Idempotency-Key on every attempt") {
  HeadRecorder g(R"({"state":"paused"})", 2);
  Client c = clientFor(g);
  c.pause("sbx-1");
  const auto heads = g.heads();
  REQUIRE(heads.size() == 3);
  auto keyOf = [](const std::string& h) {
    const std::string tag = "\r\nidempotency-key: ";
    auto at = h.find(tag);
    if (at == std::string::npos) return std::string();
    at += tag.size();
    return h.substr(at, h.find("\r\n", at) - at);
  };
  CHECK_FALSE(keyOf(heads[0]).empty());
  CHECK(keyOf(heads[1]) == keyOf(heads[0]));
  CHECK(keyOf(heads[2]) == keyOf(heads[0]));
}

// More PauseRetryable answers than the transport's ordinary retry budget (5 retries): a pause takes 35-110 s on a loaded host, so
// it is retried for a 120 s window (as in TypeScript and Python), not ~5 s. Six 503s then the 200 (~10 s of backoff). Without the
// window the 6th 503 reached the caller.
TEST_CASE("pause retries PauseRetryable beyond the ordinary retry budget") {
  HeadRecorder g(R"({"state":"paused"})", 6);
  Client c = clientFor(g);
  CHECK_NOTHROW(c.pause("sbx-1"));
  CHECK(g.heads().size() == 7);
}

// ClientOptions.idleCloseMs reaches every handle the client returns (Go ClientOptions.IdleCloseMs, Rust idle_close_ms). It was not
// an option at all: a C++ handle always idle-closed after 3 s.
TEST_CASE("ClientOptions.idleCloseMs is applied to the handles the client returns") {
  HeadRecorder g(R"({"sandboxId":"sbx-1","kind":"sandbox","state":"running"})");
  ClientOptions o;
  o.apiKey = "slr_test_x";
  o.baseUrl = g.baseUrl();
  CHECK(o.idleCloseMs == 3000);
  o.idleCloseMs = 1234;
  Client c(o);
  auto sb = c.connect("sbx-1");
  CHECK(sb->channel().idleCloseMs() == 1234);
  o.idleCloseMs = 0;
  Client off(o);
  CHECK(off.connect("sbx-1")->channel().idleCloseMs() <= 0);
}

// Client::start against a mock gateway: the fresh control URL, the idempotent
// already-running answer, and how each refusal surfaces. Loopback only.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;

namespace {

/** Answers every request with one canned status/body and records the request line. */
class CannedGateway {
 public:
  CannedGateway(int status, std::string body) : status_(status), body_(std::move(body)) {
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
  ~CannedGateway() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
  }
  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<std::string> lines() {
    std::lock_guard<std::mutex> lk(m_);
    return lines_;
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
        {
          std::lock_guard<std::mutex> lk(m_);
          lines_.push_back(head.substr(0, head.find("\r\n")));
        }
        const std::string resp = "HTTP/1.1 " + std::to_string(status_) +
                                 " X\r\nContent-Type: application/json\r\nContent-Length: " +
                                 std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" +
                                 body_;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
      }
      ::close(c);
    }
  }

  int status_;
  std::string body_;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex m_;
  std::vector<std::string> lines_;
};

Client clientFor(const CannedGateway& g) {
  ClientOptions o;
  o.apiKey = "slr_test_x";
  o.baseUrl = g.baseUrl();
  return Client(o);
}

}  // namespace

TEST_CASE("start returns the fresh control URL") {
  CannedGateway g(200, R"({"state":"running","controlUrl":"wss://gw.test/control/fresh"})");
  Client c = clientFor(g);
  CHECK(c.start("pool-a:vm-7") == "wss://gw.test/control/fresh");
  REQUIRE(g.lines().size() == 1);
  CHECK(g.lines()[0] == "POST /sandboxes/pool-a%3Avm-7/start HTTP/1.1");
}

TEST_CASE("start on an already-running sandbox still returns the URL") {
  CannedGateway g(
      200, R"({"state":"running","controlUrl":"wss://gw.test/control/cur","alreadyRunning":true})");
  Client c = clientFor(g);
  CHECK(c.start("sbx1") == "wss://gw.test/control/cur");
}

TEST_CASE("start derives the URL when the gateway omits it") {
  CannedGateway g(200, R"({"state":"running"})");
  Client c = clientFor(g);
  const std::string url = c.start("sbx1");
  CHECK(url.rfind("ws", 0) == 0);
  CHECK(url.find("/control/sbx1") != std::string::npos);
}

TEST_CASE("start surfaces each 409 code on GatewayError") {
  for (const char* code :
       {"NotStopped", "StartInProgress", "StoppedDiskMissing", "StoppedDiskUnsafe",
        "GoldenMismatch", "DiskPlatformMismatch", "DiskShaMismatch", "DiskExceedsSpec",
        "DiskOwnerMismatch", "HostCapabilityMissing", "SandboxStopped"}) {
    CannedGateway g(409, std::string("{\"code\":\"") + code +
                             "\",\"error\":\"refused\",\"state\":\"paused\",\"retryable\":false}");
    Client c = clientFor(g);
    try {
      c.start("sbx1");
      FAIL("start should have thrown for " << code);
    } catch (const GatewayError& e) {
      CHECK(e.status == 409);
      REQUIRE(e.code.has_value());
      CHECK(*e.code == code);
    }
  }
}

TEST_CASE("start maps 429, 503 and 500") {
  {
    CannedGateway g(429, R"({"error":"cap"})");
    Client c = clientFor(g);
    CHECK_THROWS_AS(c.start("s"), ConcurrencyLimitError);
  }
  {
    CannedGateway g(503, R"({"code":"NoCapacity","error":"none","retryable":true})");
    Client c = clientFor(g);
    CHECK_THROWS_AS(c.start("s"), NoCapacityError);
  }
  {
    CannedGateway g(500, R"({"code":"StartFailed","error":"x","retryable":false})");
    Client c = clientFor(g);
    try {
      c.start("s");
      FAIL("start should have thrown");
    } catch (const GatewayError& e) {
      CHECK(e.status == 500);
      CHECK(e.code.value_or("") == "StartFailed");
    }
  }
}

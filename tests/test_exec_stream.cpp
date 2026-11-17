// R4 (retest 2026-11-16): aborting a one-shot /exec stops the command only if the request carries `X-Solari-Stream: 1` (the gateway
// then starts its response early so the load balancer passes a dropped connection on). The C++ SDK did not send it. A streamed answer
// is HTTP 200 with keep-alive newlines first and the real status in "httpStatus". Loopback only.
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

/** GET /sandboxes/<id> answers a running sandbox; POST .../exec answers `execBody` (HTTP 200). Records every request head, lower-cased. */
class ExecServer {
 public:
  explicit ExecServer(std::string execBody) : execBody_(std::move(execBody)) {
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
  ~ExecServer() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
  }
  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
  std::vector<std::string> execHeads() {
    std::lock_guard<std::mutex> lk(m_);
    return execHeads_;
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
        std::transform(head.begin(), head.end(), head.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::string payload;
        if (head.rfind("post /sandboxes/sbx/exec ", 0) == 0) {
          std::lock_guard<std::mutex> lk(m_);
          execHeads_.push_back(head);
          payload = execBody_;
        } else {
          payload = R"({"sandboxId":"sbx","kind":"sandbox","state":"running","expiresAt":""})";
        }
        const std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(payload.size()) +
                                 "\r\nConnection: close\r\n\r\n" + payload;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
      }
      ::close(c);
    }
  }

  std::string execBody_;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex m_;
  std::vector<std::string> execHeads_;
};

std::shared_ptr<Sandbox> connectTo(const ExecServer& s) {
  ClientOptions o;
  o.apiKey = "slr_test_x";
  o.baseUrl = s.baseUrl();
  Client c(o);
  return c.connect("sbx");
}

}  // namespace

TEST_CASE("a one-shot exec sends X-Solari-Stream: 1") {
  ExecServer s(R"({"exitCode":0,"stdout":"hi","stderr":""})");
  auto sbx = connectTo(s);
  sbx->commands.run("echo");
  auto heads = s.execHeads();
  REQUIRE(heads.size() == 1);
  CHECK_MESSAGE(heads[0].find("\r\nx-solari-stream: 1\r\n") != std::string::npos, "head was:\n" << heads[0]);
}

TEST_CASE("a streamed answer is unwrapped into the result") {
  ExecServer s("\n\n\n{\"exitCode\":3,\"stdout\":\"out\",\"stderr\":\"err\",\"httpStatus\":200}");
  auto sbx = connectTo(s);
  CommandResult r = sbx->commands.run("sh");
  CHECK(r.exitCode == 3);
  CHECK(r.stdout_ == "out");
  CHECK(r.stderr_ == "err");
}

TEST_CASE("a streamed error status becomes the error with its real status") {
  ExecServer s("\n{\"code\":\"ExecInterrupted\",\"error\":\"parked\",\"retryable\":false,\"httpStatus\":409}");
  auto sbx = connectTo(s);
  CHECK_THROWS_AS(sbx->commands.run("sh"), ExecInterruptedError);
}

TEST_CASE("a streamed 400 ExecTimeout is still the elapsed-deadline result") {
  ExecServer s("\n{\"code\":\"ExecTimeout\",\"error\":\"deadline\",\"httpStatus\":400}");
  auto sbx = connectTo(s);
  CommandResult r = sbx->commands.run("sleep");
  CHECK(r.timedOut);
}

TEST_CASE("only a streamed request carries the header: other requests never do") {
  HttpTransport ht("k", "http://127.0.0.1:1");
  auto has = [](const std::vector<std::string>& h) { return std::find(h.begin(), h.end(), std::string("X-Solari-Stream: 1")) != h.end(); };
  CHECK(has(ht.prepare("POST", "/x", true, "{}", std::nullopt, true).headers));
  CHECK_FALSE(has(ht.prepare("POST", "/x", true, "{}", std::nullopt).headers));
}

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
  explicit HeadRecorder(std::string body) : body_(std::move(body)) {
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
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
            std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n" + body_;
        ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
      }
      ::close(c);
    }
  }

  std::string body_;
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

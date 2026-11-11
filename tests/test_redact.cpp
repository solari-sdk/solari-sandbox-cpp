#include <doctest/doctest.h>

#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include "solari/solari.hpp"

#ifdef SOLARI_WITH_WS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <memory>

#include "solari/ws_transport.hpp"
#endif

using namespace solari;

TEST_CASE("redactControlUrl removes the control URL and any ws(s) URL, keeps the rest") {
  const std::string url = "wss://api.example/control/SECRETTOKEN123";
  const std::string msg = "Expecting status 101 (Switching Protocol), got 404 status connecting to " + url;
  const std::string out = redactControlUrl(msg, url);
  CHECK(out.find("SECRETTOKEN123") == std::string::npos);
  CHECK(out.find("api.example") == std::string::npos);
  CHECK(out.find("got 404 status") != std::string::npos);
  // a form the caller's url string does not match (library normalised it): the generic scheme rule still strips it
  const std::string out2 = redactControlUrl("failed: ws://10.0.0.1:80/control/OTHERSECRET end", "");
  CHECK(out2.find("OTHERSECRET") == std::string::npos);
  CHECK(out2.find("end") != std::string::npos);
  CHECK(redactControlUrl("plain text", url) == "plain text");
}

#ifdef SOLARI_WITH_WS
// The real transport against a local server that answers the upgrade with a 404: the ConnectionError the caller sees must not
// quote the control URL (which carries the sandbox credential).
TEST_CASE("WS connect failure: ConnectionError text does not include the control URL") {
  int ls = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(ls >= 0);
  int one = 1;
  ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  REQUIRE(::bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0);
  REQUIRE(::listen(ls, 8) == 0);
  socklen_t len = sizeof(a);
  ::getsockname(ls, reinterpret_cast<sockaddr*>(&a), &len);
  const int port = ntohs(a.sin_port);
  std::atomic<bool> stop{false};
  std::thread srv([&] {
    while (!stop) {
      timeval tv{0, 200000};
      fd_set fds;
      FD_ZERO(&fds);
      FD_SET(ls, &fds);
      if (::select(ls + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
      int c = ::accept(ls, nullptr, nullptr);
      if (c < 0) continue;
      char buf[2048];
      (void)::recv(c, buf, sizeof(buf), 0);
      const char* r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      (void)::send(c, r, std::strlen(r), 0);
      ::close(c);
    }
  });
  const std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/control/SANDBOXCRED987";
  auto chan = std::make_shared<ControlChannel>();
  WsControlTransport t(url, "key", chan);
  std::string what;
  try {
    t.connect();
  } catch (const ConnectionError& e) {
    what = e.what();
  }
  stop = true;
  srv.join();
  ::close(ls);
  REQUIRE_FALSE(what.empty());
  CHECK(what.find("SANDBOXCRED987") == std::string::npos);
  CHECK(what.find("/control/") == std::string::npos);
}
#endif

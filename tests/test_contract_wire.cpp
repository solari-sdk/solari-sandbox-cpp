// Cross-language wire contract — C++ side.
//
// Asserts this SDK puts BYTE-IDENTICAL HTTP requests on the wire as the
// reference (TypeScript) SDK, for every op in the shared core surface. The
// expectation is not written here: it is read from
// tests/sdk/ts-request-fixtures.json (path injected as SOLARI_CONTRACT_FIXTURE by
// CMake), which tests/sdk/contract.test.ts regenerates from the reference SDK on
// every run. See that file for the design.
//
// WHY THIS LIVES IN sdk/cpp RATHER THAN tests/sdk: it rides the ctest job that
// already exists and already has the toolchain, so the gate cannot be dropped by
// "the contract job doesn't have a C++ compiler". The four other bindings each do
// the same inside their own suite.
//
// WHAT IT CATCHES:
//
//  1. A create field this SDK never puts on the wire. buildCreateBody() was
//     missing `lifecycle`, `resolution`, `record` AND `volumes` — four fields —
//     while its own unit test ("buildCreateBody omits unset fields") passed, because
//     that test only checked the fields it knew about. This fixture is MAXIMAL, so
//     a missing field is a diff.
//  2. A core op this SDK doesn't implement AT ALL. `coreOps` is iterated and an op
//     with no entry in the table below fails. That is how pause/resume are pinned:
//     this SDK accepted `lifecycle.onTimeout:"pause"` while offering NO WAY to
//     resume the VM it had just paused, and no per-field assertion would ever have
//     noticed an ABSENCE.
//  3. A field deleted from CreateSandboxOptions: this file sets every one of them,
//     so removing one fails the BUILD before any assertion runs. In a language
//     without reflection, the compiler is the reflection.
//
// The fake gateway speaks HTTP only. `Sandbox::resume()` ends in a real
// `connect()`, which would need a WebSocket server — IXWebSocket ships a client,
// not a server, and hand-rolling the RFC 6455 handshake (SHA-1 + base64 of the
// Sec-WebSocket-Key) for a test is not worth it. So resume points the handle at a
// dead port, expects the reconnect to fail, and asserts the handle ADOPTED the new
// control URL — which is the part this suite can honestly prove. The reconnect
// itself is covered live by tests/test_live.cpp. This is NOT a silent tolerance:
// if resume() threw before calling its hook, the URL would not change and the
// CHECK below fails; if resume() did not exist, this file would not compile.

#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;
using nlohmann::json;

#ifndef SOLARI_CONTRACT_FIXTURE
#error "SOLARI_CONTRACT_FIXTURE must point at tests/sdk/ts-request-fixtures.json"
#endif

namespace {

// Mirrors the fixture's sessionId; asserted equal below so they cannot drift.
const std::string kCanonicalId = "pool-a:vm-7:org-42.sig";
const std::string kEncId = "pool-a%3Avm-7%3Aorg-42.sig";

// A control URL on a port nothing listens on: the post-resume reconnect must fail
// FAST (connection refused) rather than hang to the 15s connect timeout.
const std::string kDeadControlUrl = "ws://127.0.0.1:1/control/" + kEncId;

struct Recorded {
  std::string method;
  std::string path;
  std::string body;
};

/** A recording HTTP server on 127.0.0.1, returning the TS fake gateway's shapes. */
class RecordingGateway {
 public:
  RecordingGateway() {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd_ >= 0);
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr("127.0.0.1");
    addr.sin_port = 0;  // kernel picks
    REQUIRE(::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    REQUIRE(::listen(fd_, 8) == 0);
    socklen_t len = sizeof(addr);
    REQUIRE(::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { loop(); });
  }

  ~RecordingGateway() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
    if (fd_ >= 0) ::close(fd_);
  }

  std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::vector<Recorded> requests() {
    std::lock_guard<std::mutex> lk(m_);
    return reqs_;
  }

 private:
  void loop() {
    while (!stop_.load()) {
      pollfd p{fd_, POLLIN, 0};
      if (::poll(&p, 1, 100) <= 0) continue;  // 100ms tick so stop_ is observed
      int c = ::accept(fd_, nullptr, nullptr);
      if (c < 0) continue;
      serve(c);
      ::close(c);
    }
  }

  void serve(int c) {
    // Read headers, then exactly Content-Length bytes of body.
    std::string buf;
    char tmp[2048];
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
      ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
      if (n <= 0) return;
      buf.append(tmp, static_cast<std::size_t>(n));
      headEnd = buf.find("\r\n\r\n");
    }
    const std::string head = buf.substr(0, headEnd);
    const std::size_t clen = contentLength(head);
    const std::size_t bodyStart = headEnd + 4;
    while (buf.size() < bodyStart + clen) {
      ssize_t n = ::recv(c, tmp, sizeof(tmp), 0);
      if (n <= 0) break;
      buf.append(tmp, static_cast<std::size_t>(n));
    }

    // "METHOD /path HTTP/1.1" — the RAW, still-escaped path is the wire.
    // Decoding it here would launder exactly the %3A-vs-: divergence this
    // contract exists to pin.
    const std::size_t sp1 = head.find(' ');
    const std::size_t sp2 = head.find(' ', sp1 + 1);
    Recorded r;
    r.method = head.substr(0, sp1);
    r.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
    r.body = buf.substr(bodyStart, clen);
    {
      std::lock_guard<std::mutex> lk(m_);
      reqs_.push_back(r);
    }

    const std::string payload = canned(r);
    const std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
        std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload;
    ::send(c, resp.data(), resp.size(), MSG_NOSIGNAL);
  }

  static std::string canned(const Recorded& r) {
    if (r.method == "POST" && r.path == "/sandboxes") {
      std::string kind = "sandbox";
      json b = json::parse(r.body, nullptr, false);
      if (b.is_object() && b.contains("kind") && b["kind"].is_string())
        kind = b["kind"].get<std::string>();
      json out = {{"sandboxId", kCanonicalId},
                  {"kind", kind},
                  {"controlUrl", kDeadControlUrl},
                  {"expiresAt", "2026-01-01T00:00:00Z"}};
      if (kind == "desktop") out["streamUrl"] = "ws://127.0.0.1:1/stream/" + kEncId;
      return out.dump();
    }
    if (r.method == "POST" && endsWith(r.path, "/pause")) {
      return json({{"state", "paused"}}).dump();
    }
    if (r.method == "POST" && endsWith(r.path, "/resume")) {
      // The session comes back on a FRESH slot, so the gateway hands back the
      // control URL to re-attach to.
      return json({{"state", "running"}, {"controlUrl", kDeadControlUrl}}).dump();
    }
    if (r.method == "GET" && r.path.rfind("/sandboxes/", 0) == 0) {
      return json({{"sandboxId", kCanonicalId},
                   {"kind", "sandbox"},
                   {"state", "running"},
                   {"expiresAt", "2026-01-01T00:00:00Z"}})
          .dump();
    }
    return json({{"ok", true}}).dump();
  }

  static bool endsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
  }

  static std::size_t contentLength(const std::string& head) {
    std::string lower;
    lower.reserve(head.size());
    for (char ch : head) lower.push_back(static_cast<char>(::tolower(ch)));
    const std::size_t at = lower.find("content-length:");
    if (at == std::string::npos) return 0;
    return static_cast<std::size_t>(std::stoul(head.substr(at + 15)));
  }

  int fd_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::mutex m_;
  std::vector<Recorded> reqs_;
};

/** The maximal create DTO, field-for-field the TS reference's. EVERY field of
 *  CreateSandboxOptions is set on purpose: deleting one breaks this build. */
CreateSandboxOptions maximalOptions() {
  CreateSandboxOptions o;
  o.template_ = "base";
  o.cpu = 2;
  o.memMb = 2048;
  o.diskGb = 10;
  o.envs = {{"FOO", "bar"}};
  o.metadata = {{"proj", "alpha"}};
  o.timeoutMs = 600000;
  o.fromSnapshot = "snap_seed";
  Lifecycle lc;
  lc.onTimeout = "pause";
  lc.autoResume = true;
  o.lifecycle = lc;
  o.volumes = {VolumeAttachment{"vol_data", "/data"}};
  return o;
}

json loadContract() {
  // HARD FAIL, never skip. The fixture is committed; its absence means the
  // checkout is broken or the reference suite never ran — either way this test
  // proves nothing, and "proves nothing" must not read as "passed".
  std::ifstream in(SOLARI_CONTRACT_FIXTURE);
  REQUIRE_MESSAGE(in.good(), "wire contract unreadable: " SOLARI_CONTRACT_FIXTURE);
  json c = json::parse(in, nullptr, false);
  REQUIRE_MESSAGE(!c.is_discarded(), "wire contract is not valid JSON");
  REQUIRE_MESSAGE(c.contains("coreOps"), "wire contract carries no coreOps — not a contract");
  REQUIRE_MESSAGE(c.contains("requests"), "wire contract carries no requests — not a contract");
  return c;
}

}  // namespace

TEST_CASE("wire contract: every core op matches the reference SDK") {
  json contract = loadContract();
  CHECK(contract["sessionId"].get<std::string>() == kCanonicalId);

  RecordingGateway gw;
  ClientOptions co;
  co.apiKey = "key-test";
  co.baseUrl = gw.baseUrl();
  Client client(co);

  std::shared_ptr<Sandbox> sb;

  // THE OP TABLE. Every op the contract names must appear here, bound to the real
  // SDK call that performs it.
  std::map<std::string, std::function<void()>> ops = {
      {"createSandbox", [&] { sb = client.create(maximalOptions()); }},
      {"createDesktop",
       [&] {
         CreateSandboxOptions o = maximalOptions();
         o.template_ = "default";
         o.resolution = "1280x720";
         o.record = true;
         auto d = client.createDesktop(o);
         d->close();
       }},
      {"get", [&] { client.get(kCanonicalId); }},
      {"pause",
       [&] {
         sb->pause();
         // Asserted here, not at the end: kill() closes the channel too, so a
         // check after the loop would pass whether or not pause touched it.
         CHECK_FALSE(sb->connected());
       }},
      {"resume",
       [&] {
         // The reconnect cannot succeed against an HTTP-only fake (see the file
         // header). What IS provable here: resume called its hook and adopted the
         // control URL of the fresh slot.
         try {
           sb->resume();
         } catch (const Error&) {
           // expected: no WS server on the far end
         }
         CHECK(sb->controlUrl() == kDeadControlUrl);
       }},
      {"kill", [&] { sb->kill(); }},
  };

  // A CHECKLIST THAT FAILS CLOSED.
  std::vector<std::string> have;
  for (const auto& kv : ops) have.push_back(kv.first);
  std::vector<std::string> want = contract["coreOps"].get<std::vector<std::string>>();
  std::sort(have.begin(), have.end());
  std::sort(want.begin(), want.end());
  CHECK_MESSAGE(have == want,
                "op table does not match the contract's coreOps. An op in the contract "
                "with no entry here is a surface this binding is missing.");

  for (const auto& name : contract["coreOps"]) {
    const std::string op = name.get<std::string>();
    CAPTURE(op);
    auto it = ops.find(op);
    if (it == ops.end()) {
      FAIL_CHECK("NOT IMPLEMENTED in the C++ SDK op table: " << op);
      continue;
    }
    const std::size_t before = gw.requests().size();
    it->second();
    auto made = gw.requests();
    // An op that emitted nothing must never compare equal to anything.
    REQUIRE(made.size() == before + 1);
    const Recorded& got = made[before];
    const json& exp = contract["requests"][op];

    CHECK(got.method == exp["method"].get<std::string>());
    CHECK(got.path == exp["path"].get<std::string>());

    json gotBody = got.body.empty() ? json() : json::parse(got.body, nullptr, false);
    json wantBody = exp.contains("body") ? exp["body"] : json();
    CHECK(gotBody == wantBody);
  }
}

TEST_CASE("wire contract: buildCreateBody carries every create field") {
  // A tighter, faster guard on the same fact as above, so a create-DTO
  // regression names itself instead of surfacing as a body diff.
  json body = buildCreateBody(maximalOptions(), SandboxKind::Sandbox);
  for (const char* field : {"template", "kind", "cpu", "memMb", "diskGb", "envs", "metadata",
                            "timeoutMs", "fromSnapshot", "lifecycle", "volumes"}) {
    CAPTURE(field);
    CHECK(body.contains(field));
  }
  CHECK(body["lifecycle"]["onTimeout"] == "pause");
  CHECK(body["lifecycle"]["autoResume"] == true);
  CHECK(body["volumes"][0]["volumeId"] == "vol_data");
  CHECK(body["volumes"][0]["path"] == "/data");

  CreateSandboxOptions o = maximalOptions();
  o.resolution = "1280x720";
  o.record = true;
  json d = buildCreateBody(o, SandboxKind::Desktop);
  CHECK(d["kind"] == "desktop");
  CHECK(d["resolution"] == "1280x720");
  CHECK(d["record"] == true);
}

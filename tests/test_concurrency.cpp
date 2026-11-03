// Concurrency, file transfer and cancellation, against an in-process scripted control channel (no socket): every frame the
// SDK sends is recorded, and a script answers it from a detached thread, the way the rx thread would.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

std::string reply(const std::string& id, json result) {
  return json{{"id", id}, {"ok", true}, {"result", std::move(result)}}.dump() + "\n";
}
std::string fail(const std::string& id, const std::string& msg) {
  return json{{"id", id}, {"ok", false}, {"error", msg}}.dump() + "\n";
}

struct Rig {
  // answer(request) -> raw frames to feed back; an empty vector says nothing.
  using Script = std::function<std::vector<std::string>(const json& req)>;

  Sandbox sbx;
  std::mutex m;
  std::condition_variable cv;
  std::vector<json> sent;
  Script script;

  static CreateSandboxResponse resp() {
    CreateSandboxResponse r;
    r.sandboxId = "sb";
    r.controlUrl = "ws://127.0.0.1:1/control/sb";
    return r;
  }
  static SandboxConfig cfg() {
    SandboxConfig c;
    c.callTimeoutMs = 800;
    return c;
  }

  std::atomic<int> inflight{0};
  std::atomic<bool> dead{false};

  ~Rig() {
    // Detached reply threads and the SDK's own background threads (a cancelled run's SIGKILL) may still be sending.
    dead = true;
    for (int i = 0; i < 100 && inflight.load() > 0; i++) std::this_thread::sleep_for(20ms);
    sbx.channel().setSend(nullptr);
  }

  explicit Rig(Script s) : sbx(resp(), cfg()), script(std::move(s)) {
    sbx.channel().setSend([this](const std::string& line) {
      if (dead) throw ConnectionError("rig is gone");
      json req = json::parse(line);
      {
        std::lock_guard<std::mutex> lk(m);
        sent.push_back(req);
        cv.notify_all();
      }
      std::vector<std::string> frames = script(req);
      if (frames.empty()) return;
      ControlChannel* ch = &sbx.channel();
      inflight++;
      std::thread([this, ch, frames] {
        for (const auto& f : frames) ch->onMessage(f);
        inflight--;
      }).detach();
    });
    sbx.channel().setConnected(true);
  }

  std::vector<std::string> methods() {
    std::lock_guard<std::mutex> lk(m);
    std::vector<std::string> out;
    for (auto& r : sent) out.push_back(r["method"].get<std::string>());
    return out;
  }
  bool waitFor(std::size_t n, std::chrono::milliseconds d = 3000ms) {
    std::unique_lock<std::mutex> lk(m);
    return cv.wait_for(lk, d, [&] { return sent.size() >= n; });
  }
  json at(std::size_t i) {
    std::lock_guard<std::mutex> lk(m);
    return sent.at(i);
  }
};

std::string idOf(const json& req) { return req["id"].get<std::string>(); }

}  // namespace

TEST_CASE("a callback may call back into the SDK: onData from inside a callback no longer deadlocks") {
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread([done] {
    Rig rig([](const json& req) -> std::vector<std::string> {
      if (req["method"] == "cmd.start") {
        return {reply(idOf(req), {{"cmdId", "c1"}}),
                R"({"type":"cmd.data","cmdId":"c1","stream":"stdout","base64":"aGk="})" "\n",
                R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n"};
      }
      return {reply(idOf(req), {{"ok", true}})};
    });
    auto h = std::make_shared<CommandHandle>(rig.sbx.commands.start("x", {}));
    // The first subscriber's callback subscribes another: the replay used to run under the state's mutex.
    h->onData([h](const std::string&, const std::string&) { h->onData([](const std::string&, const std::string&) {}); });
    h->wait();
    done->store(true);
  }).detach();
  for (int i = 0; i < 60 && !done->load(); i++) std::this_thread::sleep_for(50ms);
  CHECK(done->load());
}

TEST_CASE("a callback may make a call: it no longer runs on the rx thread that must deliver the reply") {
  // One rx thread, as with the real socket: it feeds the data frame, waits for the callback's cmd.stdin request, then feeds its reply.
  std::mutex m;
  std::condition_variable cv;
  std::vector<json> sent;
  Sandbox sbx(Rig::resp(), Rig::cfg());
  sbx.channel().setSend([&](const std::string& line) {
    std::lock_guard<std::mutex> lk(m);
    sent.push_back(json::parse(line));
    cv.notify_all();
  });
  sbx.channel().setConnected(true);

  std::thread starter([&] {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, 3s, [&] { return !sent.empty(); });
    std::string id = sent[0]["id"].get<std::string>();
    lk.unlock();
    sbx.channel().onMessage(reply(id, {{"cmdId", "c1"}}));
  });
  CommandHandle h = sbx.commands.start("cat", {});
  starter.join();

  std::atomic<bool> callOk{false};
  std::atomic<bool> callDone{false};
  h.onData([&](const std::string&, const std::string&) {
    if (callDone.exchange(true)) return;
    try {
      h.stdin_write("yes\n");
      callOk = true;
    } catch (const Error&) {
    }
  });
  std::thread rx([&] {
    sbx.channel().onMessage(R"({"type":"cmd.data","cmdId":"c1","stream":"stdout","base64":"cHJvbXB0Pg=="})" "\n");
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, 3s, [&] { return sent.size() >= 2; });
    if (sent.size() < 2) return;
    std::string id = sent[1]["id"].get<std::string>();
    lk.unlock();
    sbx.channel().onMessage(reply(id, {{"ok", true}}));
    sbx.channel().onMessage(R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n");
  });
  rx.join();
  CHECK(h.waitFor(3000).has_value());
  CHECK(callOk.load());
}

TEST_CASE("wait() can be called more than once, from several threads") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") {
      return {reply(idOf(req), {{"cmdId", "c1"}}), R"({"type":"cmd.exit","cmdId":"c1","exitCode":5})" "\n"};
    }
    return {reply(idOf(req), {{"ok", true}})};
  });
  CommandHandle h = rig.sbx.commands.start("true", {});
  std::vector<std::thread> ts;
  std::atomic<int> good{0};
  for (int i = 0; i < 4; i++) ts.emplace_back([&] { if (h.wait() == 5) good++; });
  for (auto& t : ts) t.join();
  CHECK(good == 4);
  CHECK(h.wait() == 5);
}

TEST_CASE("callbacks have seen all output by the time wait() returns") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") {
      return {reply(idOf(req), {{"cmdId", "c1"}}),
              R"({"type":"cmd.data","cmdId":"c1","stream":"stdout","base64":"YQ=="})" "\n",
              R"({"type":"cmd.data","cmdId":"c1","stream":"stdout","base64":"Yg=="})" "\n",
              R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n"};
    }
    return {reply(idOf(req), {{"ok", true}})};
  });
  std::string got;
  std::mutex gm;
  CommandOptions o;
  o.onStdout = [&](const std::string& s) {
    std::this_thread::sleep_for(20ms);  // a slow consumer
    std::lock_guard<std::mutex> lk(gm);
    got += s;
  };
  CommandResult r = rig.sbx.commands.run("x", o);
  CHECK(r.exitCode == 0);
  std::lock_guard<std::mutex> lk(gm);
  CHECK(got == "ab");
  CHECK(r.stdout_ == "ab");
}

// ---- files -------------------------------------------------------------------------------------------------

TEST_CASE("upload sends 4 MiB chunks, truncating only the first") {
  Rig rig([](const json& req) -> std::vector<std::string> { return {reply(idOf(req), {{"ok", true}})}; });
  rig.sbx.files.upload("/f", std::string(9u << 20, 'x'));
  REQUIRE(rig.methods().size() == 3);
  for (std::size_t i = 0; i < 3; i++) {
    json q = rig.at(i);
    CHECK(q["method"] == "fs.upload");
    CHECK(q["params"]["offset"].get<std::size_t>() == i * (4u << 20));
    CHECK(q["params"]["truncate"].get<bool>() == (i == 0));
  }
}

TEST_CASE("upload of an empty file is one zero-length call") {
  Rig rig([](const json& req) -> std::vector<std::string> { return {reply(idOf(req), {{"ok", true}})}; });
  rig.sbx.files.upload("/e", "");
  REQUIRE(rig.methods().size() == 1);
  CHECK(rig.at(0)["params"]["base64"] == "");
  CHECK(rig.at(0)["params"]["truncate"] == true);
}

TEST_CASE("read loops over fs.download chunks until eof") {
  std::string content;
  for (std::size_t i = 0; i < (9u << 20); i++) content.push_back(static_cast<char>('a' + (i % 26)));
  Rig rig([&](const json& req) -> std::vector<std::string> {
    std::size_t off = req["params"]["offset"].get<std::size_t>();
    std::size_t len = req["params"]["length"].get<std::size_t>();
    std::size_t end = std::min(off + len, content.size());
    return {reply(idOf(req), {{"base64", base64Encode(content.substr(off, end - off))},
                              {"eof", end == content.size()},
                              {"size", content.size()},
                              {"nextOffset", end}})};
  });
  CHECK(rig.sbx.files.read("/big") == content);
  CHECK(rig.methods() == std::vector<std::string>{"fs.download", "fs.download", "fs.download"});
}

TEST_CASE("read falls back to fs.read on a guest without fs.download") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "fs.download") return {fail(idOf(req), "unknown method: fs.download")};
    return {reply(idOf(req), {{"base64", base64Encode("hi")}})};
  });
  CHECK(rig.sbx.files.read("/f") == "hi");
  CHECK(rig.methods() == std::vector<std::string>{"fs.download", "fs.read"});
}

TEST_CASE("write refuses more than 32 MiB before sending anything") {
  Rig rig([](const json& req) -> std::vector<std::string> { return {reply(idOf(req), {{"ok", true}})}; });
  std::string msg;
  try {
    rig.sbx.files.write("/f", std::string((32u << 20) + 1, 'x'));
  } catch (const Error& e) {
    msg = e.what();
  }
  CHECK(msg == "file too large for a single call (33554433 bytes, max 33554432): use files.upload()/files.download() or uploadUrl()");
  CHECK(rig.methods().empty());
  rig.sbx.files.write("/f", "small");
  CHECK(rig.methods().size() == 1);
}

// ---- cancellation ----------------------------------------------------------------------------------------------

TEST_CASE("a cancelled run stops the guest process: SIGTERM and stdin eof now, SIGKILL after the grace") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") return {reply(idOf(req), {{"cmdId", "c1"}})};
    return {reply(idOf(req), {{"ok", true}})};  // the command never exits
  });
  auto token = std::make_shared<CancelToken>();
  CommandOptions o;
  o.cancel = token;
  std::thread canceller([&] { std::this_thread::sleep_for(150ms); token->cancel(); });
  CHECK_THROWS_AS(rig.sbx.commands.run("sleep", o), CancelledError);
  canceller.join();
  REQUIRE(rig.waitFor(3));
  CHECK(rig.at(1)["method"] == "cmd.kill");
  CHECK(rig.at(1)["params"]["signal"] == 15);
  CHECK(rig.at(2)["method"] == "cmd.stdin");
  CHECK(rig.at(2)["params"]["eof"] == true);
  CHECK(rig.at(2)["params"]["base64"] == "");
  std::this_thread::sleep_for(500ms);
  CHECK(rig.methods().size() == 3);  // not before the grace
  REQUIRE(rig.waitFor(4, 3000ms));
  CHECK(rig.at(3)["method"] == "cmd.kill");
  CHECK(rig.at(3)["params"]["signal"] == 9);
}

TEST_CASE("a cancellable run that finishes on its own sends no cancel calls") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") {
      return {reply(idOf(req), {{"cmdId", "c1"}}), R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n"};
    }
    return {reply(idOf(req), {{"ok", true}})};
  });
  CommandOptions o;
  o.cancel = std::make_shared<CancelToken>();
  CommandResult r = rig.sbx.commands.run("true", o);
  CHECK(r.exitCode == 0);
  std::this_thread::sleep_for(300ms);
  CHECK(rig.methods() == std::vector<std::string>{"cmd.start"});
}

TEST_CASE("a cancelled code run asks the kernel to stop and tolerates a guest without code.interrupt") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "code.run") return {};  // never answers
    if (req["method"] == "code.interrupt") return {fail(idOf(req), "unknown method: code.interrupt")};
    return {reply(idOf(req), {{"ok", true}})};
  });
  auto token = std::make_shared<CancelToken>();
  RunCodeOptions o;
  o.contextId = "k1";
  o.cancel = token;
  std::thread canceller([&] { std::this_thread::sleep_for(150ms); token->cancel(); });
  CHECK_THROWS_AS(rig.sbx.code.run("while True: pass", o), CancelledError);
  canceller.join();
  REQUIRE(rig.waitFor(2));
  CHECK(rig.at(1)["method"] == "code.interrupt");
  CHECK(rig.at(1)["params"]["contextId"] == "k1");
  // Today's guest does not know code.interrupt: that must not tear the channel down or surface anywhere.
  std::this_thread::sleep_for(200ms);
  CHECK(rig.sbx.channel().connected());
  rig.sbx.files.mkdir("/still-works");
}

#ifdef SOLARI_WITH_WS
#include <ixwebsocket/IXWebSocketServer.h>

// Concurrent connect() calls each opened a socket: the last one won and the others leaked, reader thread and all.
TEST_CASE("concurrent connect() calls open one socket") {
  std::atomic<int> opens{0};
  const int port = 18400 + static_cast<int>(::getpid() % 400);
  ix::WebSocketServer server(port, "127.0.0.1");
  server.setOnClientMessageCallback([&](std::shared_ptr<ix::ConnectionState>, ix::WebSocket&, const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Open) opens++;
  });
  auto res = server.listen();
  REQUIRE(res.first);
  server.start();

  CreateSandboxResponse r;
  r.sandboxId = "sb";
  r.controlUrl = "ws://127.0.0.1:" + std::to_string(port) + "/control/sb";
  SandboxConfig c;
  c.apiKey = "k";
  Sandbox sbx(r, c);
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; i++) ts.emplace_back([&] { sbx.connect(); });
  for (auto& t : ts) t.join();
  std::this_thread::sleep_for(200ms);
  CHECK(opens.load() == 1);
  CHECK(sbx.connected());
  sbx.close();
  server.stop();
}
#endif

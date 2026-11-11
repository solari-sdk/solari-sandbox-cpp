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
  static SandboxConfig cfg(SessionHooks hooks = {}) {
    SandboxConfig c;
    c.callTimeoutMs = 800;
    c.hooks = std::move(hooks);
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

  explicit Rig(Script s, SessionHooks hooks = {}) : sbx(resp(), cfg(std::move(hooks))), script(std::move(s)) {
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

// ---- exact output bytes (retest 2026-11-10) -----------------------------------------------------------------------------

TEST_CASE("stdout and stderr keep their exact bytes over the live connection, apart from each other") {
  // stdout "\xff\x00ab", stderr "\xfe\x00!": base64 of those bytes
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") {
      return {reply(idOf(req), {{"cmdId", "c1"}}),
              R"({"type":"cmd.data","cmdId":"c1","stream":"stdout","base64":"/wBhYg=="})" "\n",
              R"({"type":"cmd.data","cmdId":"c1","stream":"stderr","base64":"/gAh"})" "\n",
              R"({"type":"cmd.exit","cmdId":"c1","exitCode":2})" "\n"};
    }
    return {reply(idOf(req), {{"ok", true}})};
  });
  CommandOptions o;
  o.onStdout = [](const std::string&) {};  // a callback forces the live connection
  CommandResult r = rig.sbx.commands.run("x", o);
  CHECK(r.stdoutBytes == std::string("\xff\x00" "ab", 4));
  CHECK(r.stderrBytes == std::string("\xfe\x00!", 3));
  CHECK(r.stdout_ != r.stdoutBytes);  // the text form replaced the invalid byte
}

TEST_CASE("the /exec answer's exact bytes: base64 per stream, the text as the fallback, a bad base64 falls back") {
  CommandResult both = detail::parseCommandResult(
      json::parse(R"({"exitCode":1,"stdout":"\ufffd","stderr":"\ufffd","stdoutBase64":"/wD+","stderrBase64":"/w=="})"));
  CHECK(both.stdoutBytes == std::string("\xff\x00\xfe", 3));
  CHECK(both.stderrBytes == std::string("\xff", 1));
  CommandResult text = detail::parseCommandResult(json::parse(R"({"exitCode":1,"stdout":"h\u00e9llo","stderr":"oops \u00e9"})"));
  CHECK(text.stdoutBytes == "h\xc3\xa9llo");
  CHECK(text.stderrBytes == "oops \xc3\xa9");
  CommandResult bad = detail::parseCommandResult(
      json::parse(R"({"exitCode":1,"stdout":"ok","stderr":"bad","stdoutBase64":"!!","stderrBase64":"!!"})"));
  CHECK(bad.stdoutBytes == "ok");
  CHECK(bad.stderrBytes == "bad");
}

TEST_CASE("every refusal before git runs throws, and sends nothing") {
  Rig rig([](const json& req) -> std::vector<std::string> { return {reply(idOf(req), {{"ok", true}})}; });
  GitCheckoutOptions co;
  co.cwd = "/r";
  CHECK_THROWS_AS(rig.sbx.git.checkout("--upload-pack=evil", co), Error);
  GitRemoteOptions push;
  push.cwd = "/r";
  push.remote = "-evil";
  CHECK_THROWS_AS(rig.sbx.git.push(push), Error);
  GitRemoteOptions pull;
  pull.cwd = "/r";
  pull.branch = "--force";
  CHECK_THROWS_AS(rig.sbx.git.pull(pull), Error);
  GitCloneOptions cred;
  cred.username = "u\nx";
  cred.password = "p";
  CHECK_THROWS_AS(rig.sbx.git.clone("https://h/r.git", cred), Error);
  CHECK(rig.methods().empty());
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


// ---- commands.run on a connected shared sandbox ------------------------------------------------------------------------------

namespace {
std::string sharedRefusal(const std::string& id) {
  return json{{"id", id},
              {"ok", false},
              {"error", {{"code", "NotAvailableOnSharedIsolation"},
                         {"message", "cmd.start is not available on a shared context: it operates on the whole VM, not this context"}}}}
             .dump() + "\n";
}
}  // namespace

// A shared sandbox refuses cmd.start but runs commands over REST: run() on a CONNECTED shared sandbox used to return the refusal.
// It falls back to REST, and remembers the sandbox is shared so later runs do not try the stream again.
TEST_CASE("commands.run on a connected shared sandbox falls back to REST /exec") {
  std::atomic<int> rest{0};
  SessionHooks hooks;
  hooks.exec = [&](const std::string&, const ExecOneShotRequest& req) {
    rest++;
    return CommandResult{0, "hi from " + req.cmd, ""};
  };
  Rig rig([](const json& req) {
    if (req["method"] == "cmd.start") return std::vector<std::string>{sharedRefusal(idOf(req))};
    return std::vector<std::string>{reply(idOf(req), json::object())};
  }, hooks);
  CommandOptions o;
  o.args = {"-c", "echo hi"};
  for (int i = 0; i < 3; i++) {
    CommandResult r = rig.sbx.commands.run("sh", o);
    CHECK(r.exitCode == 0);
    CHECK(r.stdout_ == "hi from sh");
  }
  CHECK(rest.load() == 3);
  int starts = 0;
  for (const auto& m : rig.methods()) starts += m == "cmd.start";
  CHECK_MESSAGE(starts == 1, "the refusal is remembered: cmd.start sent once");
}

TEST_CASE("a streaming run on a shared sandbox still reports the refusal") {
  std::atomic<int> rest{0};
  SessionHooks hooks;
  hooks.exec = [&](const std::string&, const ExecOneShotRequest&) {
    rest++;
    return CommandResult{0, "", ""};
  };
  Rig rig([](const json& req) { return std::vector<std::string>{sharedRefusal(idOf(req))}; }, hooks);
  CommandOptions o;
  o.args = {"-c", "echo hi"};
  o.onStdout = [](const std::string&) {};
  CHECK_THROWS_AS(rig.sbx.commands.run("sh", o), ActionError);
  CHECK(rest.load() == 0);
}

// A run with per-command env or user cannot be served by REST (the one-shot route runs under the session env only): on a shared
// sandbox it reports the refusal rather than silently running without its env.
TEST_CASE("a run with env or user on a shared sandbox reports the refusal, never goes over REST") {
  std::atomic<int> rest{0};
  SessionHooks hooks;
  hooks.exec = [&](const std::string&, const ExecOneShotRequest&) {
    rest++;
    return CommandResult{0, "", ""};
  };
  Rig rig([](const json& req) { return std::vector<std::string>{sharedRefusal(idOf(req))}; }, hooks);
  CommandOptions withEnv;
  withEnv.args = {"-c", "echo $X"};
  withEnv.env = {{"X", "1"}};
  CHECK_THROWS_AS(rig.sbx.commands.run("sh", withEnv), ActionError);
  CommandOptions withUser;
  withUser.args = {"-c", "id"};
  withUser.user = "nobody";
  CHECK_THROWS_AS(rig.sbx.commands.run("sh", withUser), ActionError);
  CHECK(rest.load() == 0);
}

TEST_CASE("runCode with callbacks asks the guest to stream and delivers output while the cell runs") {
  // One rx thread, as with the real socket: it feeds the first output frame, then WAITS until the callback has seen it before it
  // feeds the rest and the result. A character split across two frames arrives whole, and the result's complete text is not
  // delivered a second time beyond the unflushed tail.
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

  std::string out, err;
  bool gotFirst = false;
  bool liveBeforeRest = false;
  RunCodeOptions o;
  o.onStdout = [&](const std::string& s) {
    std::lock_guard<std::mutex> lk(m);
    out += s;
    gotFirst = true;
    cv.notify_all();
  };
  o.onStderr = [&](const std::string& s) { err += s; };

  std::thread rx([&] {
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, 3s, [&] { return !sent.empty(); });
    if (sent.empty()) return;
    std::string id = sent[0]["id"].get<std::string>();
    lk.unlock();
    const std::string euro = "\xE2\x82\xAC";  // 3 bytes, split across two frames
    auto frame = [&](const char* stream, const std::string& bytes) {
      return std::string("{\"id\":\"") + id + "\",\"stream\":\"" + stream + "\",\"data\":\"" + base64Encode(bytes) + "\"}\n";
    };
    sbx.channel().onMessage(frame("stdout", "tick1 "));
    sbx.channel().onMessage(frame("stdout", euro.substr(0, 2)));
    lk.lock();
    cv.wait_for(lk, 3s, [&] { return gotFirst; });
    liveBeforeRest = gotFirst;  // the callback fired before the rest and the result were fed
    lk.unlock();
    sbx.channel().onMessage(frame("stdout", euro.substr(2)));
    sbx.channel().onMessage(frame("stderr", "warn\n"));
    sbx.channel().onMessage(reply(id, {{"results", json::array({{{"type", "stdout"}, {"text", "tick1 " + euro}},
                                                                {{"type", "stderr"}, {"text", "warn\n"}},
                                                                {{"type", "stdout"}, {"text", "tail"}}})}}));
  });
  RunCodeResult r = sbx.runCode("print()", o);
  rx.join();
  {
    std::lock_guard<std::mutex> lk(m);
    CHECK(sent[0]["method"] == "code.run");
    CHECK(sent[0]["params"]["stream"] == true);
  }
  CHECK(liveBeforeRest);
  CHECK(out == "tick1 \xE2\x82\xAC" "tail");
  CHECK(err == "warn\n");
  CHECK(r.results.size() == 3);
}

TEST_CASE("a command that exits before its handle exists leaves no close hook, so the socket can idle-close") {
  // The cmd.exit frame rides in the same message as the cmd.start reply, so it is buffered before the handle exists. Registering
  // the frame handlers replays it and its handler unregisters the close hook; the hook must therefore be registered BEFORE the
  // handlers, or it is registered after that unregister and leaks (retest B-N1 / C4).
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start")
      return {R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n" + reply(idOf(req), {{"cmdId", "c1"}})};
    return {};
  });
  CommandHandle h = rig.sbx.commands.start("true", {});
  auto code = h.waitFor(3000);
  REQUIRE(code.has_value());
  CHECK(rig.sbx.channel().streamCloserCount() == 0);
}

// --- idle close (retest C-17) -------------------------------------------------------------------------------------------------
// An open control socket counts as activity at the gateway, so one left open keeps an idle sandbox from pausing or expiring on time.
// The channel closes the socket after idleCloseMs with nothing using it and reopens it by itself on the next call.

static bool becomes(const std::function<bool()>& cond, std::chrono::milliseconds d = 3000ms) {
  auto end = std::chrono::steady_clock::now() + d;
  while (std::chrono::steady_clock::now() < end) {
    if (cond()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return cond();
}

TEST_CASE("an idle control socket is closed, and the next call reopens it") {
  ControlChannel ch;
  std::atomic<int> closes{0}, reopens{0};
  ch.setSend([&](const std::string& line) {
    json req = json::parse(line);
    std::string id = req["id"].get<std::string>();
    std::thread([&ch, id] { ch.onMessage(reply(id, {{"ok", true}})); }).detach();
  });
  ch.setIdleCloseHook([&] { closes++; });
  ch.setReconnectHook([&] {
    reopens++;
    ch.setConnected(true);
  });
  ch.setIdleCloseMs(150);
  ch.setConnected(true);
  ch.call("fs.stat", {{"path", "/"}});
  CHECK(becomes([&] { return closes.load() == 1; }));
  CHECK_FALSE(ch.connected());
  ch.call("fs.stat", {{"path", "/"}});  // reopens by itself
  CHECK(reopens.load() == 1);
  CHECK(becomes([&] { return closes.load() == 2; }));
}

TEST_CASE("a running command keeps the socket open, and it closes after the command ends") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start") return {reply(idOf(req), {{"cmdId", "c1"}})};
    return {};
  });
  rig.sbx.channel().setIdleCloseMs(150);
  CommandHandle h = rig.sbx.commands.start("sleep", {});
  std::this_thread::sleep_for(700ms);  // several idle windows
  CHECK(rig.sbx.channel().connected());
  rig.sbx.channel().onMessage(R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n");
  REQUIRE(h.waitFor(3000).has_value());
  CHECK(becomes([&] { return !rig.sbx.channel().connected(); }));
}

TEST_CASE("a fast command (exit already buffered when the handle is built) does not keep the socket open") {
  Rig rig([](const json& req) -> std::vector<std::string> {
    if (req["method"] == "cmd.start")
      return {R"({"type":"cmd.exit","cmdId":"c1","exitCode":0})" "\n" + reply(idOf(req), {{"cmdId", "c1"}})};
    return {};
  });
  rig.sbx.channel().setIdleCloseMs(150);
  CommandHandle h = rig.sbx.commands.start("true", {});
  REQUIRE(h.waitFor(3000).has_value());
  CHECK(becomes([&] { return !rig.sbx.channel().connected(); }));
}

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;
using nlohmann::json;

// ---------------------------------------------------------------------------
// util / base64
// ---------------------------------------------------------------------------
TEST_CASE("base64 round-trips + matches known vectors") {
  CHECK(base64Encode(std::string("hi")) == "aGk=");
  CHECK(base64Encode(std::string("hello\n")) == "aGVsbG8K");
  CHECK(base64Decode("aGVsbG8K") == "hello\n");
  CHECK(base64Decode("b3Jw") == "orp");
  CHECK(base64Decode("YWJj") == "abc");
}

TEST_CASE("encodeURIComponent matches JS semantics") {
  CHECK(encodeURIComponent("p@ss word") == "p%40ss%20word");
  CHECK(encodeURIComponent("a-_.!~*'()") == "a-_.!~*'()");  // unreserved
}

// ---------------------------------------------------------------------------
// Control channel: frame splitting + orphan buffering
// ---------------------------------------------------------------------------
static void wire(ControlChannel& ch) {
  ch.setSend([](const std::string&) {});
  ch.setConnected(true);
}

TEST_CASE("frame splitter: two frames in one buffer") {
  ControlChannel ch;
  wire(ch);
  std::vector<std::string> got;
  ch.onFrame("cmd.data", "c1", [&](const json& f) {
    got.push_back(base64Decode(f["base64"].get<std::string>()));
  });
  ch.onMessage(
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base64\":"
      "\"aGk=\"}\n"
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base64\":"
      "\"eW8=\"}\n");
  REQUIRE(got.size() == 2);
  CHECK(got[0] == "hi");
  CHECK(got[1] == "yo");
}

TEST_CASE("frame splitter: one frame split across two reads") {
  ControlChannel ch;
  wire(ch);
  std::vector<std::string> got;
  ch.onFrame("cmd.data", "c1", [&](const json& f) {
    got.push_back(base64Decode(f["base64"].get<std::string>()));
  });
  ch.onMessage("{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base");
  CHECK(got.empty());  // nothing dispatched until the newline arrives
  ch.onMessage("64\":\"YWJj\"}\n");
  REQUIRE(got.size() == 1);
  CHECK(got[0] == "abc");
}

TEST_CASE("orphan async frames are buffered then flushed on registration") {
  ControlChannel ch;
  wire(ch);
  // Frames arrive BEFORE the handler is registered (guest streams before the
  // cmd.start reply).
  ch.onMessage(
      "{\"type\":\"cmd.data\",\"cmdId\":\"c2\",\"stream\":\"stdout\",\"base64\":"
      "\"b3Jw\"}\n");
  ch.onMessage("{\"type\":\"cmd.exit\",\"cmdId\":\"c2\",\"exitCode\":7}\n");

  std::vector<std::string> data;
  int exit = -1;
  ch.onFrame("cmd.data", "c2", [&](const json& f) {
    data.push_back(base64Decode(f["base64"].get<std::string>()));
  });
  ch.onFrame("cmd.exit", "c2",
             [&](const json& f) { exit = f["exitCode"].get<int>(); });
  REQUIRE(data.size() == 1);
  CHECK(data[0] == "orp");
  CHECK(exit == 7);
}

TEST_CASE("orphan replay keeps order against a live frame that arrives mid-replay") {
  // Regression: onFrame used to register the handler BEFORE replaying parked
  // frames, so a live frame delivered by the rx thread during the replay could
  // overtake them (stdout "worldhello "). Deterministic: the handler holds the
  // replay open on its first call while the live frame is fed.
  ControlChannel ch;
  wire(ch);
  auto frame = [](const char* b64) {
    return std::string("{\"type\":\"cmd.data\",\"cmdId\":\"c3\",\"stream\":\"stdout\","
                       "\"base64\":\"") + b64 + "\"}\n";
  };
  ch.onMessage(frame("YQ=="));  // "a", parked: no handler yet

  std::mutex m;
  std::condition_variable cv;
  bool firstSeen = false, release = false;
  int calls = 0;
  std::string got;
  std::thread reg([&] {
    ch.onFrame("cmd.data", "c3", [&](const json& f) {
      std::unique_lock<std::mutex> lk(m);
      if (++calls == 1) {
        firstSeen = true;
        cv.notify_all();
        cv.wait(lk, [&] { return release; });  // hold the replay open
      }
      got += base64Decode(f["base64"].get<std::string>());
    });
  });
  {
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return firstSeen; });
  }
  ch.onMessage(frame("Yg=="));  // "b": live frame while the replay is in flight
  {
    std::lock_guard<std::mutex> lk(m);
    release = true;
  }
  cv.notify_all();
  reg.join();
  std::lock_guard<std::mutex> lk(m);
  CHECK(got == "ab");
}

TEST_CASE("onStreamClose after the channel is closed fires immediately") {
  // The registration race: a caller registers the stream closer AFTER the
  // cmd.start reply returns. If the channel tore down in that window, failAll()
  // has already drained streamClosers_, so a late store would sit unreferenced
  // and the frame-based exit-wait would block forever. onStreamClose must
  // instead invoke the callback synchronously when the channel is already down.
  ControlChannel ch;
  wire(ch);
  ch.close();
  std::exception_ptr got;
  bool fired = false;
  ch.onStreamClose("cmd_late", [&](std::exception_ptr e) {
    got = e;
    fired = true;
  });
  REQUIRE(fired);
  bool isConnErr = false;
  try {
    std::rethrow_exception(got);
  } catch (const ConnectionError&) {
    isConnErr = true;
  } catch (...) {
  }
  CHECK(isConnErr);
}

TEST_CASE("non-JSON lines are ignored, not fatal") {
  ControlChannel ch;
  wire(ch);
  bool called = false;
  ch.onFrame("cmd.data", "c9", [&](const json&) { called = true; });
  ch.onMessage("not json at all\n\n   \n");
  CHECK_FALSE(called);
}

// ---------------------------------------------------------------------------
// commands.run one-shot: a JSON 404 is a gateway answer, not "route unavailable"; timedOut and recordingUrl are kept
// ---------------------------------------------------------------------------
static Sandbox sandboxWithExec(std::function<CommandResult(const std::string&, const ExecOneShotRequest&)> exec) {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  resp.controlUrl = "ws://127.0.0.1:1/control/sb";
  resp.recordingUrl = "https://rec.example/r/1";
  SandboxConfig cfg;
  cfg.callTimeoutMs = 200;
  cfg.hooks.exec = std::move(exec);
  return Sandbox(resp, cfg);
}

TEST_CASE("a JSON 404 from /exec reaches the caller instead of falling back to the control channel") {
  Sandbox sbx = sandboxWithExec([](const std::string&, const ExecOneShotRequest&) -> CommandResult {
    throw GatewayError(404, "Not found", json{{"error", "Not found"}});
  });
  CHECK_THROWS_AS(sbx.commands.run("true"), GatewayError);
}

TEST_CASE("a plain-text 404 (route not served) still falls back to the control channel") {
  Sandbox sbx = sandboxWithExec([](const std::string&, const ExecOneShotRequest&) -> CommandResult {
    throw GatewayError(404, "Gateway request failed with status 404", json());
  });
  // The fallback needs a control channel, which this handle does not have: whatever it throws, it is not the 404.
  bool gateway404 = false;
  try {
    sbx.commands.run("true");
  } catch (const GatewayError&) {
    gateway404 = true;
  } catch (...) {
  }
  CHECK_FALSE(gateway404);
}

TEST_CASE("timedOut survives the one-shot result; recordingUrl reaches the handle") {
  Sandbox sbx = sandboxWithExec([](const std::string&, const ExecOneShotRequest&) {
    CommandResult r;
    r.exitCode = -1;
    r.timedOut = true;
    return r;
  });
  CommandResult r = sbx.commands.run("sleep", {});
  CHECK(r.timedOut);
  CHECK(r.exitCode == -1);
  REQUIRE(sbx.recordingUrl().has_value());
  CHECK(*sbx.recordingUrl() == "https://rec.example/r/1");
}

// ---------------------------------------------------------------------------
// commands.run: full result assembly from a scripted frame sequence
// ---------------------------------------------------------------------------
TEST_CASE("commands.run assembles {exitCode,stdout,stderr} from frames") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  resp.controlUrl = "ws://x/control/sb";
  SandboxConfig cfg;
  cfg.callTimeoutMs = 5000;  // no exec hook -> WS path
  Sandbox sbx(resp, cfg);

  std::mutex mm;
  std::condition_variable cvv;
  std::vector<json> sent;
  sbx.channel().setSend([&](const std::string& s) {
    std::lock_guard<std::mutex> lk(mm);
    sent.push_back(json::parse(s));
    cvv.notify_all();
  });
  sbx.channel().setConnected(true);

  CommandResult result;
  std::thread t([&] {
    CommandOptions o;
    o.args = {"hello"};
    result = sbx.commands.run("echo", o);
  });

  // Wait for the cmd.start request to be sent (pending is registered by then).
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(2), [&] { return !sent.empty(); });
  }
  REQUIRE(sent.size() == 1);
  CHECK(sent[0]["method"] == "cmd.start");
  CHECK(sent[0]["params"]["cmd"] == "echo");
  std::string id = sent[0]["id"].get<std::string>();

  // Reply with the cmdId, then stream data + exit.
  sbx.channel().onMessage("{\"id\":\"" + id +
                          "\",\"ok\":true,\"result\":{\"cmdId\":\"c1\"}}\n");
  sbx.channel().onMessage(
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base64\":"
      "\"aGVsbG8K\"}\n");
  sbx.channel().onMessage(
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stderr\",\"base64\":"
      "\"ZXJy\"}\n");
  sbx.channel().onMessage("{\"type\":\"cmd.exit\",\"cmdId\":\"c1\",\"exitCode\":0}\n");

  t.join();
  CHECK(result.exitCode == 0);
  CHECK(result.stdout_ == "hello\n");
  CHECK(result.stderr_ == "err");
}

// The guest can emit cmd.data / cmd.exit BEFORE the cmd.start reply is consumed (PROTOCOL.md section 2).
// With onStdout/onStderr set, that early output must still be in the CommandResult.
TEST_CASE("commands.run keeps output that beat the cmd.start reply when callbacks are set") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  resp.controlUrl = "ws://x/control/sb";
  SandboxConfig cfg;
  cfg.callTimeoutMs = 5000;
  Sandbox sbx(resp, cfg);

  std::mutex mm;
  std::condition_variable cvv;
  std::vector<json> sent;
  sbx.channel().setSend([&](const std::string& s) {
    std::lock_guard<std::mutex> lk(mm);
    sent.push_back(json::parse(s));
    cvv.notify_all();
  });
  sbx.channel().setConnected(true);

  std::string cbOut, cbErr;
  CommandResult result;
  std::thread t([&] {
    CommandOptions o;
    o.onStdout = [&](const std::string& d) { cbOut += d; };
    o.onStderr = [&](const std::string& d) { cbErr += d; };
    result = sbx.commands.run("x", o);
  });
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(2), [&] { return !sent.empty(); });
  }
  REQUIRE(sent.size() == 1);
  std::string id = sent[0]["id"].get<std::string>();

  // One message: output + exit first (orphans: no handler yet), the reply last.
  sbx.channel().onMessage(
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base64\":\"ZWFybHktb3V0Cg==\"}\n"
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stderr\",\"base64\":\"ZWFybHktZXJyCg==\"}\n"
      "{\"type\":\"cmd.exit\",\"cmdId\":\"c1\",\"exitCode\":3}\n"
      "{\"id\":\"" + id + "\",\"ok\":true,\"result\":{\"cmdId\":\"c1\"}}\n");
  t.join();
  CHECK(result.exitCode == 3);
  CHECK(result.stdout_ == "early-out\n");
  CHECK(result.stderr_ == "early-err\n");
  CHECK(cbOut == "early-out\n");
  CHECK(cbErr == "early-err\n");
}

// timeoutMs is a command deadline on the WebSocket path too: the command is killed and the result is exit -1
// with the output so far (it used to be honoured only by the REST /exec fast path).
TEST_CASE("commands.run enforces timeoutMs on the WS path") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  resp.controlUrl = "ws://x/control/sb";
  SandboxConfig cfg;
  cfg.callTimeoutMs = 5000;
  Sandbox sbx(resp, cfg);

  std::mutex mm;
  std::condition_variable cvv;
  std::vector<json> sent;
  sbx.channel().setSend([&](const std::string& s) {
    std::lock_guard<std::mutex> lk(mm);
    sent.push_back(json::parse(s));
    cvv.notify_all();
  });
  sbx.channel().setConnected(true);

  CommandResult result;
  std::thread t([&] {
    CommandOptions o;
    o.timeoutMs = 80;
    result = sbx.commands.run("sleep", o);
  });
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(2), [&] { return !sent.empty(); });
  }
  REQUIRE(sent.size() >= 1);
  std::string id = sent[0]["id"].get<std::string>();
  sbx.channel().onMessage(
      "{\"id\":\"" + id + "\",\"ok\":true,\"result\":{\"cmdId\":\"c1\"}}\n"
      "{\"type\":\"cmd.data\",\"cmdId\":\"c1\",\"stream\":\"stdout\",\"base64\":\"cGFydGlhbAo=\"}\n");
  // The command never exits: run() must kill it after timeoutMs. Answer the cmd.kill RPC.
  std::string killId;
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(3), [&] { return sent.size() >= 2; });
    REQUIRE(sent.size() >= 2);
    CHECK(sent[1]["method"] == "cmd.kill");
    CHECK(sent[1]["params"]["cmdId"] == "c1");
    CHECK(sent[1]["params"]["signal"] == 9);
    killId = sent[1]["id"].get<std::string>();
  }
  sbx.channel().onMessage("{\"id\":\"" + killId + "\",\"ok\":true,\"result\":{\"ok\":true}}\n");
  t.join();
  CHECK(result.exitCode == -1);
  CHECK(result.stdout_ == "partial\n");
}

TEST_CASE("channel close wakes a command's exit-wait (no hang)") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  SandboxConfig cfg;
  cfg.callTimeoutMs = 5000;
  Sandbox sbx(resp, cfg);
  std::mutex mm;
  std::condition_variable cvv;
  std::vector<json> sent;
  sbx.channel().setSend([&](const std::string& s) {
    std::lock_guard<std::mutex> lk(mm);
    sent.push_back(json::parse(s));
    cvv.notify_all();
  });
  sbx.channel().setConnected(true);

  bool threw = false;
  std::thread t([&] {
    try {
      sbx.commands.run("sleep", {});
    } catch (const ConnectionError&) {
      threw = true;
    }
  });
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(2), [&] { return !sent.empty(); });
  }
  std::string id = sent[0]["id"].get<std::string>();
  sbx.channel().onMessage("{\"id\":\"" + id +
                          "\",\"ok\":true,\"result\":{\"cmdId\":\"c1\"}}\n");
  // Drop the channel while the command is awaiting cmd.exit.
  sbx.channel().onClose(1006, "gone");
  t.join();
  CHECK(threw);
}

// ---------------------------------------------------------------------------
// files: fs.list / fs.stat wire shape ({dir}, {modTimeMs})
// ---------------------------------------------------------------------------
// Drive a single blocking `channel.call` RPC: run it on a worker, wait for the
// request to be sent, then feed the scripted reply.
template <typename Fn>
static json driveRpc(Sandbox& sbx, Fn&& op, const json& result) {
  std::mutex mm;
  std::condition_variable cvv;
  std::vector<json> sent;
  sbx.channel().setSend([&](const std::string& s) {
    std::lock_guard<std::mutex> lk(mm);
    sent.push_back(json::parse(s));
    cvv.notify_all();
  });
  sbx.channel().setConnected(true);
  json captured;
  std::thread t([&] { op(); });
  {
    std::unique_lock<std::mutex> lk(mm);
    cvv.wait_for(lk, std::chrono::seconds(2), [&] { return !sent.empty(); });
    captured = sent[0];
  }
  std::string id = captured["id"].get<std::string>();
  json reply = {{"id", id}, {"ok", true}, {"result", result}};
  sbx.channel().onMessage(reply.dump() + "\n");
  t.join();
  return captured;
}

TEST_CASE("files.list parses {name,dir,size} entries") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  SandboxConfig cfg;
  Sandbox sbx(resp, cfg);
  std::vector<FsEntry> entries;
  json req = driveRpc(
      sbx, [&] { entries = sbx.files.list("/tmp"); },
      {{"entries",
        json::array({json{{"name", "a.txt"}, {"dir", false}, {"size", 12}},
                     json{{"name", "sub"}, {"dir", true}, {"size", 0}}})}});
  CHECK(req["method"] == "fs.list");
  CHECK(req["params"]["path"] == "/tmp");
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].name == "a.txt");
  CHECK(entries[0].dir == false);
  CHECK(entries[0].size == 12);
  CHECK(entries[1].dir == true);
}

TEST_CASE("files.stat parses {name,dir,size,mode,modTimeMs}") {
  CreateSandboxResponse resp;
  resp.sandboxId = "sb";
  SandboxConfig cfg;
  Sandbox sbx(resp, cfg);
  FsStat st;
  driveRpc(
      sbx, [&] { st = sbx.files.stat("/tmp/a.txt"); },
      {{"name", "a.txt"}, {"dir", false}, {"size", 42}, {"mode", 420},
       {"modTimeMs", 1700000000000LL}});
  CHECK(st.name == "a.txt");
  CHECK(st.dir == false);
  CHECK(st.size == 42);
  CHECK(st.mode == 420);  // 0o644
  CHECK(st.modTimeMs == 1700000000000LL);
}

// ---------------------------------------------------------------------------
// code.run chart flattening
// ---------------------------------------------------------------------------
TEST_CASE("code.run flattens results[].chart into top-level charts") {
  json reply = {
      {"results",
       json::array(
           {json{{"type", "stdout"}, {"text", "hi\n"}},
            json{{"type", "result"},
                 {"png", "BASE64PNG"},
                 {"chart",
                  {{"type", "line"},
                   {"title", "T"},
                   {"xLabel", "x"},
                   {"elements", json::array({1, 2, 3})}}}},
            json{{"type", "result"},
                 {"chart", {{"type", "bar"}, {"elements", json::array()}}}}})}};

  std::string streamed;
  RunCodeResult r =
      flattenCodeResult(reply, [&](const std::string& s) { streamed += s; });

  CHECK(r.results.size() == 3);
  REQUIRE(r.charts.size() == 2);
  CHECK(r.charts[0].type == "line");
  CHECK(r.charts[0].title.value() == "T");
  CHECK(r.charts[0].elements.is_array());
  CHECK(r.charts[0].elements.size() == 3);
  CHECK(r.charts[1].type == "bar");
  CHECK(streamed == "hi\n");
  CHECK_FALSE(r.error.has_value());
}

TEST_CASE("code.run surfaces an error field") {
  json reply = {{"results", json::array()},
                {"error", {{"name", "ValueError"}, {"message", "boom"}}}};
  RunCodeResult r = flattenCodeResult(reply);
  REQUIRE(r.error.has_value());
  CHECK((*r.error)["message"] == "boom");
}

// ---------------------------------------------------------------------------
// git: parsers + arg construction (mirrors git.test.mjs)
// ---------------------------------------------------------------------------
struct GitCall {
  std::string cmd;
  std::vector<std::string> args;
  std::optional<std::string> cwd;
  std::map<std::string, std::string> env;
};

// NUL-terminated records, the shape of `git status -z`.
static std::string nulRecords(const std::vector<std::string>& recs) {
  std::string out;
  for (const auto& r : recs) {
    out += r;
    out.push_back('\0');
  }
  return out;
}

TEST_CASE("git.status parses branch, ahead/behind, staged/modified/untracked") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    if (!o.args.empty() && o.args[0] == "status")
      return {0,
              nulRecords({"## main...origin/main [ahead 2, behind 1]", "M  staged.ts", " M modified.ts", "MM both.ts", "?? new.txt"}),
              ""};
    return {0, "", ""};
  });
  GitStatus st = g.status("/repo");
  CHECK(st.branch == "main");
  CHECK(st.upstream.value_or("") == "origin/main");
  CHECK(st.ahead == 2);
  CHECK(st.behind == 1);
  CHECK(st.detached == false);
  CHECK(st.staged == std::vector<std::string>{"staged.ts", "both.ts"});
  CHECK(st.modified == std::vector<std::string>{"modified.ts", "both.ts"});
  CHECK(st.untracked == std::vector<std::string>{"new.txt"});
  CHECK(st.clean == false);
  CHECK(calls[0].cwd.value() == "/repo");
  CHECK(calls[0].args == std::vector<std::string>{"status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"});
}

TEST_CASE("git.status clean tree + detached HEAD") {
  GitOps g([&](const std::string&, const CommandOptions& o) -> CommandResult {
    if (!o.args.empty() && o.args[0] == "status") return {0, nulRecords({"## HEAD (no branch)"}), ""};
    return {0, "", ""};
  });
  GitStatus st = g.status();
  CHECK(st.detached == true);
  CHECK(st.clean == true);
  CHECK(st.branch == "");
}

TEST_CASE("git.status handles a fresh repo with no commits") {
  GitOps g([&](const std::string&, const CommandOptions& o) -> CommandResult {
    if (!o.args.empty() && o.args[0] == "status")
      return {0, nulRecords({"## No commits yet on trunk", "?? a.txt"}), ""};
    return {0, "", ""};
  });
  GitStatus st = g.status();
  CHECK(st.branch == "trunk");
  CHECK(st.untracked == std::vector<std::string>{"a.txt"});
}

TEST_CASE("git.branches parses name/commit/current from the stable format") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    if (!o.args.empty() && o.args[0] == "for-each-ref")
      return {0, "*\037main\037abc1234\037origin/main\n \037dev\037def5678\037\n", ""};
    return {0, "", ""};
  });
  auto branches = g.branches("/repo");
  REQUIRE(branches.size() == 2);
  CHECK(branches[0].name == "main");
  CHECK(branches[0].commit == "abc1234");
  CHECK(branches[0].current == true);
  CHECK(branches[1].name == "dev");
  CHECK(branches[1].commit == "def5678");
  CHECK(branches[1].current == false);
  CHECK(branches[0].upstream.value_or("") == "origin/main");
  CHECK_FALSE(branches[1].upstream.has_value());
  bool usesFormat = false;
  for (const auto& a : calls[0].args)
    if (a.find("%(refname:short)") != std::string::npos) usesFormat = true;
  CHECK(usesFormat);
}

TEST_CASE("git.log parses unit-separated commit records") {
  GitOps g([&](const std::string&, const CommandOptions& o) -> CommandResult {
    if (!o.args.empty() && o.args[0] == "log")
      return {0,
              "h1\037Ada\037ada@x\0372026-01-01T00:00:00+00:00\037first\037body 1\nline 2\n\036\n"
              "h2\037Lin\037lin@x\0372026-01-02T00:00:00+00:00\037second\037\n\036\n",
              ""};
    return {0, "", ""};
  });
  auto log = g.log(GitLogOptions{std::string("/repo"), 2});
  REQUIRE(log.size() == 2);
  CHECK(log[0].hash == "h1");
  CHECK(log[0].author == "Ada");
  CHECK(log[0].email == "ada@x");
  CHECK(log[0].date == "2026-01-01T00:00:00+00:00");
  CHECK(log[0].message == "first");
  CHECK(log[0].body == "body 1\nline 2");
  CHECK(log[1].message == "second");
  CHECK(log[1].body == "");
}

TEST_CASE("git.commit sets identity, then resolves the new hash") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    bool hasRevParse = false;
    for (const auto& a : o.args)
      if (a == "rev-parse") hasRevParse = true;
    if (hasRevParse) return {0, "deadbeef\n", ""};
    return {0, "", ""};
  });
  GitCommitOptions opts;
  opts.cwd = "/repo";
  opts.author = "Ada";
  opts.email = "ada@x";
  opts.all = true;
  auto res = g.commit("msg", opts);
  CHECK(res.hash == "deadbeef");
  const GitCall* commitCall = nullptr;
  for (const auto& c : calls)
    for (const auto& a : c.args)
      if (a == "commit") commitCall = &c;
  REQUIRE(commitCall != nullptr);
  CHECK(commitCall->args ==
        std::vector<std::string>{"-c", "user.name=Ada", "-c", "user.email=ada@x",
                                 "commit", "-m", "msg", "-a"});
}

static bool argvHas(const std::vector<std::string>& args, const std::string& needle) {
  for (const auto& a : args)
    if (a.find(needle) != std::string::npos) return true;
  return false;
}

TEST_CASE("git.clone sends credentials by env + an origin-bound helper, never in the URL or argv") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    return {0, "", ""};
  });
  GitCloneOptions opts;
  opts.branch = "main";
  opts.depth = 1;
  opts.username = "u";
  opts.password = "p@ss word";
  opts.path = "dest";
  opts.cwd = "/work";
  g.clone("https://github.com/acme/repo.git", opts);
  REQUIRE(calls.size() == 1);
  const auto& a = calls[0].args;
  CHECK(calls[0].cwd.value() == "/work");
  CHECK(a[0] == "-c");
  CHECK(a[1] == "credential.helper=");
  CHECK(a[2] == "-c");
  CHECK(a[3].rfind("credential.https://github.com.helper=!f()", 0) == 0);
  CHECK(std::vector<std::string>(a.begin() + 4, a.end()) ==
        std::vector<std::string>{"clone", "--depth", "1", "--branch", "main", "--",
                                 "https://github.com/acme/repo.git", "dest"});
  CHECK(calls[0].env.at("SOLARI_GIT_USERNAME") == "u");
  CHECK(calls[0].env.at("SOLARI_GIT_PASSWORD") == "p@ss word");
  CHECK(calls[0].env.at("GIT_TERMINAL_PROMPT") == "0");
  CHECK_FALSE(argvHas(a, "p@ss"));
  CHECK_FALSE(argvHas(a, "p%40ss"));
  CHECK_FALSE(argvHas(a, "u:"));
}

TEST_CASE("git.clone puts -- before a user URL and sends no helper without creds") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    return {0, "", ""};
  });
  GitCloneOptions opts;
  opts.path = "dest";
  g.clone("--upload-pack=touch /tmp/pwned", opts);
  CHECK(calls[0].args ==
        std::vector<std::string>{"clone", "--", "--upload-pack=touch /tmp/pwned", "dest"});
  CHECK(calls[0].env.empty());
}

TEST_CASE("git credentials with line breaks are refused; the password is masked in errors") {
  GitOps g([&](const std::string&, const CommandOptions&) -> CommandResult { return {0, "", ""}; });
  GitCloneOptions bad;
  bad.username = "u";
  bad.password = std::string("a\nurl=https://evil");
  CHECK_THROWS_AS(g.clone("https://h/r.git", bad), Error);
  GitOps g2([&](const std::string&, const CommandOptions&) -> CommandResult {
    return {128, "", "fatal: unable to access: hunter2 rejected"};
  });
  GitCloneOptions ok;
  ok.username = "u";
  ok.password = "hunter2";
  try {
    g2.clone("https://h/r.git", ok);
    FAIL("expected a throw");
  } catch (const Error& e) {
    std::string m = e.what();
    CHECK(m.find("git clone failed") != std::string::npos);
    CHECK(m.find("hunter2") == std::string::npos);
  }
}

TEST_CASE("GitOps::authFor binds the helper to the origin and ignores ssh remotes") {
  auto a = GitOps::authFor("https://git.example.com:8443/acme/repo.git", "u", "p");
  REQUIRE(a.ok);
  CHECK(a.cfg[3].rfind("credential.https://git.example.com:8443.helper=!f()", 0) == 0);
  CHECK_FALSE(GitOps::authFor("git@github.com:acme/repo.git", "u", "p").ok);
  CHECK_FALSE(GitOps::authFor("https://github.com/x.git", "", "").ok);
  // Userinfo already in the URL is not part of the origin.
  CHECK(GitOps::authFor("https://old:cred@github.com/x.git", "u", "p").cfg[3].rfind(
            "credential.https://github.com.helper=", 0) == 0);
}

TEST_CASE("git.add is a no-op on empty paths and uses -- separator otherwise") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    return {0, "", ""};
  });
  g.add({});
  CHECK(calls.empty());
  g.add({"a.ts", "-weird.ts"}, "/repo");
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].args ==
        std::vector<std::string>{"add", "--", "a.ts", "-weird.ts"});
}

TEST_CASE("git surfaces a non-zero exit as an error with stderr") {
  GitOps g([&](const std::string&, const CommandOptions&) -> CommandResult {
    return {128, "", "fatal: not a git repository"};
  });
  bool threw = false;
  try {
    g.status("/nope");
  } catch (const Error& e) {
    threw = true;
    std::string msg = e.what();
    CHECK(msg.find("not a git repository") != std::string::npos);
    CHECK(msg.find("git status failed (exit 128)") != std::string::npos);
  }
  CHECK(threw);
}

TEST_CASE("git.push resolves the remote URL, binds the helper to its origin, keeps the secret out of argv") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    if (!o.args.empty() && o.args[0] == "remote")
      return {0, "https://git.example.com:8443/acme/repo.git\n", ""};
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.username = "u";
  opts.password = "tok";
  opts.cwd = "/repo";
  opts.branch = "main";
  g.push(opts);
  REQUIRE(calls.size() == 2);
  CHECK(calls[0].args == std::vector<std::string>{"remote", "get-url", "--", "origin"});
  CHECK(calls[0].env.empty());
  const auto& push = calls[1].args;
  CHECK(push[0] == "-c");
  CHECK(push[1] == "credential.helper=");
  CHECK(push[3].rfind("credential.https://git.example.com:8443.helper=!f()", 0) == 0);
  CHECK(std::vector<std::string>(push.begin() + 4, push.end()) ==
        std::vector<std::string>{"push", "--", "origin", "main"});
  CHECK(calls[1].env.at("SOLARI_GIT_PASSWORD") == "tok");
  CHECK_FALSE(argvHas(push, "tok"));
}

TEST_CASE("git.push without creds is a plain push; an ssh remote gets no helper") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    if (!o.args.empty() && o.args[0] == "remote") return {0, "git@github.com:acme/repo.git\n", ""};
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.cwd = "/repo";
  g.push(opts);
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].args == std::vector<std::string>{"push", "--", "origin"});
  CHECK(calls[0].env.empty());
  GitRemoteOptions withCreds = opts;
  withCreds.username = "u";
  withCreds.password = "p";
  g.pull(withCreds);
  REQUIRE(calls.size() == 3);
  CHECK(calls[2].args == std::vector<std::string>{"pull", "--", "origin"});
  CHECK(calls[2].env.empty());
}

TEST_CASE("git.pull shares the auth path with op=pull and a custom remote") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    if (!o.args.empty() && o.args[0] == "remote")
      return {0, "https://github.com/acme/repo.git\n", ""};
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.remote = "upstream";
  opts.username = "u";
  opts.password = "p";
  g.pull(opts);
  CHECK(calls[0].args == std::vector<std::string>{"remote", "get-url", "--", "upstream"});
  const auto& pull = calls.back().args;
  CHECK(std::vector<std::string>(pull.begin() + 4, pull.end()) ==
        std::vector<std::string>{"pull", "--", "upstream"});
  CHECK(calls.back().env.at("SOLARI_GIT_USERNAME") == "u");
}

TEST_CASE("git push/pull/checkout refuse option-like remotes, branches and refs") {
  int ran = 0;
  GitOps g([&](const std::string&, const CommandOptions&) -> CommandResult {
    ++ran;
    return {0, "", ""};
  });
  GitRemoteOptions r1;
  r1.remote = "--receive-pack=x";
  CHECK_THROWS_AS(g.push(r1), Error);
  GitRemoteOptions r2;
  r2.branch = "--rebase";
  CHECK_THROWS_AS(g.pull(r2), Error);
  CHECK_THROWS_AS(g.checkout("--orphan=x"), Error);
  CHECK(ran == 0);
}

TEST_CASE("git.checkout switches refs; create adds -b") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd, o.env});
    return {0, "", ""};
  });
  GitCheckoutOptions co;
  co.cwd = "/repo";
  g.checkout("dev", co);
  CHECK(calls[0].args == std::vector<std::string>{"checkout", "dev", "--"});
  GitCheckoutOptions cc;
  cc.cwd = "/repo";
  cc.create = true;
  g.checkout("feature", cc);
  CHECK(calls[1].args == std::vector<std::string>{"checkout", "-b", "feature", "--"});
}

// ---------------------------------------------------------------------------
// REST create request shape
// ---------------------------------------------------------------------------
TEST_CASE("buildCreateBody omits unset fields, keeps template + kind") {
  CreateSandboxOptions o;
  o.template_ = "base";
  json body = buildCreateBody(o);
  CHECK(body["template"] == "base");
  CHECK(body["kind"] == "sandbox");
  CHECK_FALSE(body.contains("cpu"));
  CHECK_FALSE(body.contains("envs"));
  CHECK_FALSE(body.contains("fromSnapshot"));

  CreateSandboxOptions o2;
  o2.template_ = "code";
  o2.cpu = 4;
  o2.memMb = 8192;
  o2.envs = {{"K", "V"}};
  json b2 = buildCreateBody(o2);
  CHECK(b2["cpu"] == 4);
  CHECK(b2["memMb"] == 8192);
  CHECK(b2["envs"]["K"] == "V");
}

TEST_CASE("HttpTransport builds the POST /sandboxes request shape") {
  HttpTransport ht("k", "http://gw/");
  json body = buildCreateBody([] {
    CreateSandboxOptions o;
    o.template_ = "base";
    return o;
  }());
  PreparedRequest r =
      ht.prepare("POST", "/sandboxes", true, body.dump(), std::string("idem-123"));
  CHECK(r.url == "http://gw/sandboxes");  // trailing slash stripped
  CHECK(r.method == "POST");
  CHECK(r.hasBody);
  auto has = [&](const std::string& h) {
    for (const auto& x : r.headers)
      if (x == h) return true;
    return false;
  };
  CHECK(has("Authorization: Bearer k"));
  CHECK(has("Accept: application/json"));
  CHECK(has("Content-Type: application/json"));
  CHECK(has("Idempotency-Key: idem-123"));
  // Body carries the create shape.
  json parsed = json::parse(r.body);
  CHECK(parsed["template"] == "base");
  CHECK(parsed["kind"] == "sandbox");
}

TEST_CASE("HttpTransport GET omits Content-Type + Idempotency-Key") {
  HttpTransport ht("k", "https://gw");
  PreparedRequest r = ht.prepare("GET", "/sandboxes/sb", false, "", std::nullopt);
  CHECK(r.url == "https://gw/sandboxes/sb");
  CHECK_FALSE(r.hasBody);
  for (const auto& h : r.headers) {
    CHECK(h.rfind("Content-Type", 0) != 0);
    CHECK(h.rfind("Idempotency-Key", 0) != 0);
  }
}

TEST_CASE("wsOrigin swaps http->ws and https->wss") {
  CHECK(HttpTransport("k", "http://gw").wsOrigin() == "ws://gw");
  CHECK(HttpTransport("k", "https://gw.example.com").wsOrigin() ==
        "wss://gw.example.com");
}

TEST_CASE("newIdempotencyKey looks like a v4 UUID") {
  std::string k = newIdempotencyKey();
  CHECK(k.size() == 36);
  CHECK(k[14] == '4');  // version nibble
}


// Gateway status + body code -> typed error (PROTOCOL.md section 4).
TEST_CASE("throwGatewayError dispatches on the body code first") {
  enum class K { Auth, Plan, Concurrency, NoCapacity, NotStopped, Interrupted, Validation, Plain };
  struct C { const char* id; int status; const char* code; K kind; };
  const C cases[] = {
      {"400-validation", 400, "ValidationError", K::Validation},
      {"400-exec-timeout", 400, "ExecTimeout", K::Validation},
      {"400-no-code", 400, "", K::Validation},
      {"401", 401, "Unauthorized", K::Auth},
      {"402-plan", 402, "FeatureRequiresPlan", K::Plan},
      {"402-credit", 402, "InsufficientCredit", K::Plan},
      {"403-not-entitled", 403, "NotEntitled", K::Plan},
      {"403-other", 403, "Forbidden", K::Plain},
      {"404", 404, "NotFound", K::Plain},
      {"409-sandbox-stopped", 409, "SandboxStopped", K::Plain},
      {"409-not-stopped", 409, "NotStopped", K::NotStopped},
      {"409-start-in-progress", 409, "StartInProgress", K::NotStopped},
      {"409-exec-interrupted", 409, "ExecInterrupted", K::Interrupted},
      {"409-host-capability-missing", 409, "HostCapabilityMissing", K::Plain},
      {"409-snapshot-not-restorable", 409, "SnapshotNotRestorable", K::Plain},
      {"410-gone", 410, "SandboxGone", K::Plain},
      {"429-concurrency-cap", 429, "ConcurrencyLimitExceeded", K::Concurrency},
      {"429-no-code", 429, "", K::Concurrency},
      {"429-too-many-executions", 429, "TooManyExecutions", K::Plain},
      {"429-object-limit", 429, "ObjectLimit", K::Plain},
      {"500-internal", 500, "InternalError", K::Plain},
      {"501-not-supported", 501, "NotSupported", K::Plain},
      {"503-no-capacity", 503, "NoCapacity", K::NoCapacity},
      {"503-start-timeout", 503, "StartTimeout", K::NoCapacity},
      {"503-no-code", 503, "", K::NoCapacity},
      {"503-sandbox-unavailable", 503, "SandboxUnavailable", K::Plain},
      {"503-store-unavailable", 503, "StoreUnavailable", K::Plain},
      {"503-admission-unavailable", 503, "AdmissionUnavailable", K::Plain},
      {"503-dependency-unavailable", 503, "DependencyUnavailable", K::Plain},
      {"507-insufficient-capacity", 507, "InsufficientCapacity", K::Plain},
  };
  for (const C& c : cases) {
    INFO("errors/" << c.id);
    json body = json::object();
    if (std::string(c.code) != "") body["code"] = c.code;
    bool threw = false;
    try {
      throwGatewayError(c.status, body);
    } catch (const GatewayError& e) {
      threw = true;
      CHECK(e.status == c.status);  // the status the gateway sent is kept
      if (std::string(c.code) != "") {
        REQUIRE(e.code.has_value());
        CHECK(*e.code == c.code);
      }
      CHECK((dynamic_cast<const AuthError*>(&e) != nullptr) == (c.kind == K::Auth));
      CHECK((dynamic_cast<const PlanError*>(&e) != nullptr) == (c.kind == K::Plan));
      CHECK((dynamic_cast<const ConcurrencyLimitError*>(&e) != nullptr) == (c.kind == K::Concurrency));
      CHECK((dynamic_cast<const NoCapacityError*>(&e) != nullptr) == (c.kind == K::NoCapacity));
      CHECK((dynamic_cast<const SandboxNotStoppedError*>(&e) != nullptr) == (c.kind == K::NotStopped));
      CHECK((dynamic_cast<const ExecInterruptedError*>(&e) != nullptr) == (c.kind == K::Interrupted));
      CHECK((dynamic_cast<const ValidationError*>(&e) != nullptr) == (c.kind == K::Validation));
    }
    CHECK(threw);
  }
}

// `state` rides along on a NotStopped refusal when the gateway reports it, and is absent otherwise.
TEST_CASE("SandboxNotStoppedError carries the sandbox state from the body") {
  try {
    throwGatewayError(409, json{{"code", "NotStopped"}, {"error", "not stopped"}, {"state", "paused"}});
    FAIL("expected a throw");
  } catch (const SandboxNotStoppedError& e) {
    CHECK(e.status == 409);
    REQUIRE(e.state.has_value());
    CHECK(*e.state == "paused");
  }
  try {
    throwGatewayError(409, json{{"code", "StartInProgress"}});
    FAIL("expected a throw");
  } catch (const SandboxNotStoppedError& e) {
    CHECK(!e.state.has_value());
  }
}

// ---------------------------------------------------------------------------
// errors mapping
// ---------------------------------------------------------------------------
TEST_CASE("throwGatewayError maps status codes to typed errors") {
  CHECK_THROWS_AS(throwGatewayError(401, json::object()), AuthError);
  CHECK_THROWS_AS(throwGatewayError(402, json::object()), PlanError);
  CHECK_THROWS_AS(throwGatewayError(429, json::object()), ConcurrencyLimitError);
  CHECK_THROWS_AS(throwGatewayError(503, json::object()), NoCapacityError);
  CHECK_THROWS_AS(throwGatewayError(400, json::object()), ValidationError);
  CHECK_THROWS_AS(throwGatewayError(404, json::object()), GatewayError);
  // body-level no_capacity
  CHECK_THROWS_AS(throwGatewayError(500, json{{"code", "no_capacity"}}),
                  NoCapacityError);
}

// ---------------------------------------------------------------------------
// create body shape (kind: sandbox vs desktop)
// ---------------------------------------------------------------------------
TEST_CASE("buildCreateBody stamps kind:\"desktop\" for a desktop VM") {
  CreateSandboxOptions opts;
  opts.template_ = "default";
  opts.cpu = 2;
  opts.memMb = 4096;

  json body = buildCreateBody(opts, SandboxKind::Desktop);
  // The gateway keys desktop-ness off this exact field/value.
  CHECK(body["kind"] == "desktop");
  CHECK(body["template"] == "default");
  CHECK(body["cpu"] == 2);
  CHECK(body["memMb"] == 4096);
  // Unset fields stay off the wire.
  CHECK(body.contains("diskGb") == false);
  CHECK(body.contains("fromSnapshot") == false);
}

TEST_CASE("buildCreateBody defaults to kind:\"sandbox\"") {
  CreateSandboxOptions opts;
  opts.template_ = "base";

  json sandboxBody = buildCreateBody(opts, SandboxKind::Sandbox);
  CHECK(sandboxBody["kind"] == "sandbox");
  // The default argument must agree with the explicit Sandbox kind.
  CHECK(buildCreateBody(opts)["kind"] == "sandbox");
}

TEST_CASE("Sandbox surfaces streamUrl for a desktop, none for a sandbox") {
  SandboxConfig config;
  config.apiKey = "k";
  config.callTimeoutMs = 1000;

  CreateSandboxResponse desktopResp;
  desktopResp.sandboxId = "sbx_d";
  desktopResp.kind = "desktop";
  desktopResp.controlUrl = "wss://gw/control/sbx_d";
  desktopResp.streamUrl = "wss://gw/stream/sbx_d";
  Sandbox desktop(desktopResp, config);
  CHECK(desktop.kind() == "desktop");
  REQUIRE(desktop.streamUrl().has_value());
  CHECK(*desktop.streamUrl() == "wss://gw/stream/sbx_d");

  // A headless sandbox has no display: the gateway sends no streamUrl and the
  // handle must not invent one.
  CreateSandboxResponse sandboxResp;
  sandboxResp.sandboxId = "sbx_1";
  sandboxResp.kind = "sandbox";
  sandboxResp.controlUrl = "wss://gw/control/sbx_1";
  Sandbox sandbox(sandboxResp, config);
  CHECK(sandbox.kind() == "sandbox");
  CHECK(sandbox.streamUrl().has_value() == false);
}

// A blank id builds a nearly-right "/sandboxes//exec" (double slash), so it is
// rejected where it enters rather than surfacing as a malformed request.
TEST_CASE("a blank session id is rejected at the boundary") {
  // Guard A: the shared Sandbox ctor rejects a blank id from a response.
  CreateSandboxResponse blank;
  blank.sandboxId = "";
  blank.controlUrl = "ws://127.0.0.1:1/control/";
  CHECK_THROWS_AS(Sandbox(blank, SandboxConfig{}), Error);

  CreateSandboxResponse okResp;
  okResp.sandboxId = "sbx_1";
  okResp.controlUrl = "ws://127.0.0.1:1/control/sbx_1";
  CHECK_NOTHROW(Sandbox(okResp, SandboxConfig{}));

  // Guard B: Client::connect() rejects a blank arg before any network call.
  Client client(ClientOptions{"k", "http://127.0.0.1:1"});
  CHECK_THROWS_AS(client.connect(""), Error);
  CHECK_THROWS_AS(client.connect("   "), Error);
}

// ---------------------------------------------------------------------------
// utf8Lossy: command output is text decoded as UTF-8 over the whole stream
// ---------------------------------------------------------------------------
TEST_CASE("utf8Lossy replaces invalid sequences with U+FFFD and keeps valid text and NUL") {
  const std::string fffd = "\xEF\xBF\xBD";
  CHECK(utf8Lossy("plain ascii") == "plain ascii");
  CHECK(utf8Lossy(std::string("a\0b\n", 4)) == std::string("a\0b\n", 4));
  CHECK(utf8Lossy("\xE6\xBC\xA2\xE5\xAD\x97") == "\xE6\xBC\xA2\xE5\xAD\x97");        // valid 3-byte characters
  CHECK(utf8Lossy("a\xFF" "b") == "a" + fffd + "b");                                  // a stray byte
  CHECK(utf8Lossy("ok\xE6") == "ok" + fffd);                                          // truncated at the end: one replacement
  CHECK(utf8Lossy("\xE6\xBC") == fffd);                                               // a truncated 3-byte sequence is ONE replacement
  CHECK(utf8Lossy("\xF0\x9F\x98") == fffd);                                           // and so is a truncated 4-byte one
  CHECK(utf8Lossy("\xC0\x80") == fffd + fffd);                                        // overlong forms are invalid
  CHECK(utf8Lossy("\xED\xA0\x80") == fffd + fffd + fffd);                             // a UTF-16 surrogate is invalid
  // A character split across two output chunks survives because the whole stream is decoded, not each chunk.
  std::string stream = std::string("\xE6\xBC") + std::string("\xA2");
  CHECK(utf8Lossy(stream) == "\xE6\xBC\xA2");
}

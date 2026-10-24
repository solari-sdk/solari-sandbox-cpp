#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
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
};

TEST_CASE("git.status parses branch, ahead/behind, staged/modified/untracked") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    if (!o.args.empty() && o.args[0] == "status")
      return {0,
              "## main...origin/main [ahead 2, behind 1]\n"
              "M  staged.ts\n"
              " M modified.ts\n"
              "MM both.ts\n"
              "?? new.txt\n",
              ""};
    return {0, "", ""};
  });
  GitStatus st = g.status("/repo");
  CHECK(st.branch == "main");
  CHECK(st.ahead == 2);
  CHECK(st.behind == 1);
  CHECK(st.detached == false);
  CHECK(st.staged == std::vector<std::string>{"staged.ts", "both.ts"});
  CHECK(st.modified == std::vector<std::string>{"modified.ts", "both.ts"});
  CHECK(st.untracked == std::vector<std::string>{"new.txt"});
  CHECK(st.clean == false);
  CHECK(calls[0].cwd.value() == "/repo");
  CHECK(calls[0].args == std::vector<std::string>{"status", "--porcelain=v1", "--branch"});
}

TEST_CASE("git.status clean tree + detached HEAD") {
  GitOps g([&](const std::string&, const CommandOptions& o) -> CommandResult {
    if (!o.args.empty() && o.args[0] == "status") return {0, "## HEAD (no branch)\n", ""};
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
      return {0, "## No commits yet on trunk\n?? a.txt\n", ""};
    return {0, "", ""};
  });
  GitStatus st = g.status();
  CHECK(st.branch == "trunk");
  CHECK(st.untracked == std::vector<std::string>{"a.txt"});
}

TEST_CASE("git.branches parses name/commit/current from the stable format") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    if (!o.args.empty() && o.args[0] == "branch")
      return {0, "*\037main\037abc1234\n \037dev\037def5678\n", ""};
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
  bool usesFormat = false;
  for (const auto& a : calls[0].args)
    if (a.find("%(refname:short)") != std::string::npos) usesFormat = true;
  CHECK(usesFormat);
}

TEST_CASE("git.log parses unit-separated commit records") {
  GitOps g([&](const std::string&, const CommandOptions& o) -> CommandResult {
    if (!o.args.empty() && o.args[0] == "log")
      return {0,
              "h1\037Ada\037ada@x\0372026-01-01T00:00:00+00:00\037first\n"
              "h2\037Lin\037lin@x\0372026-01-02T00:00:00+00:00\037second\n",
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
}

TEST_CASE("git.commit sets identity, then resolves the new hash") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
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

TEST_CASE("git.clone injects basic-auth creds into the https remote") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
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
  CHECK(calls[0].cwd.value() == "/work");
  std::vector<std::string> head(calls[0].args.begin(), calls[0].args.begin() + 5);
  CHECK(head == std::vector<std::string>{"clone", "--depth", "1", "--branch", "main"});
  const std::string& url = calls[0].args[5];
  CHECK(url.rfind("https://u:p%40ss%20word@github.com/", 0) == 0);
  CHECK(calls[0].args[6] == "dest");
}

TEST_CASE("GitOps::authUrl encodes userinfo") {
  CHECK(GitOps::authUrl("https://github.com/acme/repo.git", "u", "p@ss word")
            .rfind("https://u:p%40ss%20word@github.com/", 0) == 0);
  // scp-style / no scheme -> unchanged
  CHECK(GitOps::authUrl("git@github.com:acme/repo.git", "u", "p") ==
        "git@github.com:acme/repo.git");
}

TEST_CASE("git.add is a no-op on empty paths and uses -- separator otherwise") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
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

TEST_CASE("git.push builds a one-off authed insteadOf remote") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    if (o.args.size() >= 2 && o.args[0] == "remote" && o.args[1] == "get-url")
      return {0, "https://github.com/acme/repo.git\n", ""};
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.username = "u";
  opts.password = "tok";
  opts.cwd = "/repo";
  g.push(opts);
  // Last call is the push with the -c url.<authed>.insteadOf=<plain> prefix.
  const GitCall& push = calls.back();
  REQUIRE(push.args.size() >= 4);
  CHECK(push.args[0] == "-c");
  CHECK(push.args[1].rfind("url.https://u:tok@github.com/acme/repo.git.insteadOf="
                           "https://github.com/acme/repo.git",
                           0) == 0);
  CHECK(push.args[2] == "push");
  CHECK(push.args[3] == "origin");
}

TEST_CASE("git.push without creds skips the remote-url rewrite") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.cwd = "/repo";
  g.push(opts);
  // No `remote get-url` leg — just the push itself.
  REQUIRE(calls.size() == 1);
  CHECK(calls[0].args[0] == "push");
  CHECK(calls[0].args[1] == "origin");
}

TEST_CASE("git.pull shares the auth path with op=pull and a custom remote") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    if (!o.args.empty() && o.args[0] == "remote")
      return {0, "https://github.com/acme/repo.git\n", ""};
    return {0, "", ""};
  });
  GitRemoteOptions opts;
  opts.remote = "upstream";
  opts.username = "u";
  opts.password = "p";
  g.pull(opts);
  CHECK(calls[0].args[0] == "remote");
  CHECK(calls[0].args[1] == "get-url");
  CHECK(calls[0].args[2] == "upstream");
  const GitCall& pull = calls.back();
  CHECK(pull.args[0] == "-c");
  CHECK(pull.args[2] == "pull");
  CHECK(pull.args[3] == "upstream");
}

TEST_CASE("git.checkout switches refs; create adds -b") {
  std::vector<GitCall> calls;
  GitOps g([&](const std::string& cmd, const CommandOptions& o) -> CommandResult {
    calls.push_back({cmd, o.args, o.cwd});
    return {0, "", ""};
  });
  GitCheckoutOptions co;
  co.cwd = "/repo";
  g.checkout("dev", co);
  CHECK(calls[0].args.size() == 2);
  CHECK(calls[0].args[0] == "checkout");
  CHECK(calls[0].args[1] == "dev");
  GitCheckoutOptions cc;
  cc.cwd = "/repo";
  cc.create = true;
  g.checkout("feature", cc);
  CHECK(calls[1].args[0] == "checkout");
  CHECK(calls[1].args[1] == "-b");
  CHECK(calls[1].args[2] == "feature");
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

// ---------------------------------------------------------------------------
// errors mapping
// ---------------------------------------------------------------------------
TEST_CASE("throwGatewayError maps status codes to typed errors") {
  CHECK_THROWS_AS(throwGatewayError(401, json::object()), AuthError);
  CHECK_THROWS_AS(throwGatewayError(402, json::object()), PlanError);
  CHECK_THROWS_AS(throwGatewayError(429, json::object()), ConcurrencyLimitError);
  CHECK_THROWS_AS(throwGatewayError(503, json::object()), NoCapacityError);
  CHECK_THROWS_AS(throwGatewayError(400, json::object()), GatewayError);
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

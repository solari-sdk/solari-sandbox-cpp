// LIVE integration suite — C++, against staging.
//
//   cmake -S . -B build -DSOLARI_BUILD_LIVE_TESTS=ON && cmake --build build
//   SOLARI_LIVE=1 SOLARI_API_KEY=… ./build/solari_live_tests -s
//   SOLARI_BASE_URL=…   # override target (default: staging)
//
// Gated on SOLARI_LIVE at runtime so it never runs by accident, and built as a
// SEPARATE binary from the offline suite so `ctest` stays hermetic.
//
// Scope: the C++ SDK is CONTROL-PLANE ONLY. There is no screenshot/mouse/
// keyboard surface to test — a desktop here is a VM whose `kind()` is "desktop"
// and which carries a `streamUrl()`. This suite covers what exists:
// create/connect, kind, streamUrl, commands, files, code.run, git, lifecycle.
//
// Creates exactly ONE VM per case and kills it from a RAII guard's destructor,
// so the VM dies even when a REQUIRE throws. A leaked staging VM is real money
// and real pool pressure.
//
// Each SDK call is logged as a transcript:   -> call        <- result
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;

namespace {

const char* kDefaultBaseUrl = "https://api-sta.getsolari.com";

std::string envOr(const char* name, const std::string& fallback = "") {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

bool liveEnabled() { return !envOr("SOLARI_LIVE").empty(); }

/** Log `-> call`. */
void call(const std::string& what) { std::cout << "-> " << what << std::endl; }

/** Log `<- result` (truncated). */
void reply(const std::string& what) {
  std::string s = what;
  if (s.size() > 220) s = s.substr(0, 220) + "…";
  std::cout << "<- " << s << std::endl;
}

std::string bytesPreview(const std::string& b) {
  std::string out = "<" + std::to_string(b.size()) + " bytes> [";
  for (size_t i = 0; i < b.size() && i < 8; ++i) {
    if (i) out += ",";
    out += std::to_string(static_cast<unsigned char>(b[i]));
  }
  if (b.size() > 8) out += ",…";
  return out + "]";
}

/** Kills the VM on scope exit — runs even when a REQUIRE throws. */
struct VmGuard {
  Client& client;
  std::string id;
  ~VmGuard() {
    if (id.empty()) return;
    std::cout << "-> client.kill(" << id << ")  [teardown]" << std::endl;
    try {
      client.kill(id);
      std::cout << "<- ok (VM destroyed)" << std::endl;
    } catch (const std::exception& e) {
      // Never throw from a destructor; report loudly instead.
      std::cout << "<- TEARDOWN FAILED — VM " << id << " may be leaked: " << e.what()
                << std::endl;
    }
  }
};

/** Create a VM, retrying 25x / 6s on transient capacity/concurrency errors
 *  (mirrors the TS suite's retry budget). Any other error is a real bug and
 *  fails immediately.
 *
 *  Retryable cases are matched on the TYPED error, not on message text: the
 *  gateway's 503 body is "No sandbox host available", which contains neither
 *  "concurrent" nor "capacity" — a substring matcher (as the TS suite uses)
 *  silently fails to retry the single most common staging failure, warm-pool
 *  exhaustion.
 *
 *    429 ConcurrencyLimitError — the org's slots are held (other agents/tests)
 *    503 NoCapacityError       — no host free yet; the pool is refilling
 */
std::shared_ptr<Sandbox> createWithRetry(Client& client, bool desktop) {
  CreateSandboxOptions opts;
  opts.metadata = {{"suite", "live-cpp"}};
  // No template for a desktop: the gateway defaults kind:"desktop" to the
  // desktop golden — exercising that default is the point.
  if (!desktop) opts.template_ = "base";

  std::string lastErr;
  for (int i = 0; i < 25; ++i) {
    call(desktop ? "client.createDesktop({})" : "client.create({template:\"base\"})");
    try {
      auto sb = desktop ? client.createDesktop(opts) : client.create(opts);
      std::cout << "<- id=" << sb->id() << " kind=" << sb->kind()
                << " streamUrl=" << (sb->streamUrl() ? *sb->streamUrl() : "(none)")
                << std::endl;
      return sb;
    } catch (const NoCapacityError& e) {
      lastErr = e.what();
    } catch (const ConcurrencyLimitError& e) {
      lastErr = e.what();
    } catch (const std::exception& e) {
      FAIL("create failed (not a capacity/concurrency error): " << e.what());
      return nullptr;
    }
    std::cout << "<- transient (" << lastErr << "); retry " << (i + 1) << "/25 in 6s"
              << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(6));
  }
  FAIL("create: exhausted retries: " << lastErr);
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Desktop — the full C++ control-plane surface on ONE live VM.
// ---------------------------------------------------------------------------
TEST_CASE("live: desktop control-plane surface") {
  if (!liveEnabled()) {
    MESSAGE("skipping: set SOLARI_LIVE=1 (and SOLARI_API_KEY) to run live tests");
    return;
  }
  const std::string key = envOr("SOLARI_API_KEY");
  REQUIRE_MESSAGE(!key.empty(), "SOLARI_LIVE=1 but SOLARI_API_KEY is empty");

  ClientOptions co;
  co.apiKey = key;
  co.baseUrl = envOr("SOLARI_BASE_URL", kDefaultBaseUrl);
  Client client(co);

  auto sb = createWithRetry(client, /*desktop=*/true);
  REQUIRE(sb != nullptr);
  VmGuard guard{client, sb->id()};

  // --- create contract ------------------------------------------------------
  SUBCASE("") {
    CHECK(!sb->id().empty());
    CHECK(sb->kind() == "desktop");
    // streamUrl is returned by CREATE for desktop kind only (router.ts:1356).
    REQUIRE_MESSAGE(sb->streamUrl().has_value(),
                    "desktop create returned no streamUrl");
    CHECK(sb->streamUrl()->find("/stream/") != std::string::npos);
    CHECK((sb->streamUrl()->rfind("ws://", 0) == 0 ||
           sb->streamUrl()->rfind("wss://", 0) == 0));
    CHECK(!sb->controlUrl().empty());
  }

  call("sb->connect()");
  sb->connect();
  reply("ok");
  REQUIRE(sb->connected());

  // --- commands -------------------------------------------------------------
  {
    call("sb->commands.run(\"uname\", {args: [\"-s\"]})");
    CommandOptions o;
    o.args = {"-s"};
    auto r = sb->commands.run("uname", o);
    reply("exitCode=" + std::to_string(r.exitCode) + " stdout=" + r.stdout_);
    CHECK(r.exitCode == 0);
    // Real evidence: a Linux guest, not merely "didn't throw".
    CHECK(trim(r.stdout_) == "Linux");
  }
  {
    call("sb->commands.run(\"sh\", {args: [\"-c\", \"exit 7\"]})");
    CommandOptions o;
    o.args = {"-c", "exit 7"};
    auto r = sb->commands.run("sh", o);
    reply("exitCode=" + std::to_string(r.exitCode));
    CHECK(r.exitCode == 7);
  }
  {
    call("sb->commands.run(\"sh\", {args: [\"-c\", \"echo out; echo oops 1>&2\"]})");
    CommandOptions o;
    o.args = {"-c", "echo out; echo oops 1>&2"};
    auto r = sb->commands.run("sh", o);
    reply("stdout=" + r.stdout_ + " stderr=" + r.stderr_);
    CHECK(r.stderr_.find("oops") != std::string::npos);
    // The streams must not be merged.
    CHECK(r.stdout_.find("oops") == std::string::npos);
    CHECK(r.stdout_.find("out") != std::string::npos);
  }
  {
    // The orphan-frame regression: every run's stdout must arrive intact.
    call("sb->commands.run(\"sh\", [\"-c\",\"echo tokN\"])  x20");
    int ok = 0;
    for (int i = 0; i < 20; ++i) {
      CommandOptions o;
      o.args = {"-c", "echo tok" + std::to_string(i)};
      auto r = sb->commands.run("sh", o);
      REQUIRE(r.exitCode == 0);
      REQUIRE(trim(r.stdout_) == "tok" + std::to_string(i));
      ++ok;
    }
    reply(std::to_string(ok) + "/20 stdout captured, no dropped frames");
  }
  {
    // commands.start streams output and reports the exit code.
    call("sb->commands.start(\"sh\", [\"-c\", \"echo streamed; exit 3\"])");
    CommandOptions o;
    o.args = {"-c", "echo streamed; exit 3"};
    auto h = sb->commands.start("sh", o);
    std::string got;
    h.onData([&](const std::string& stream, const std::string& data) {
      if (stream == "stdout") got += data;
    });
    int code = h.wait();
    reply("exit=" + std::to_string(code) + " stdout=" + got);
    CHECK(code == 3);
    CHECK(got.find("streamed") != std::string::npos);
  }

  // --- files ----------------------------------------------------------------
  {
    call("sb->files.write(\"/tmp/cpp-live.txt\", \"hello\")");
    sb->files.write("/tmp/cpp-live.txt", "hello");
    reply("ok");

    call("sb->files.readText(\"/tmp/cpp-live.txt\")");
    auto txt = sb->files.readText("/tmp/cpp-live.txt");
    reply("\"" + txt + "\"");
    CHECK(txt == "hello");

    call("sb->files.list(\"/tmp\")");
    auto entries = sb->files.list("/tmp");
    reply(std::to_string(entries.size()) + " entries");
    const FsEntry* found = nullptr;
    for (const auto& e : entries)
      if (e.name == "cpp-live.txt") found = &e;
    REQUIRE_MESSAGE(found != nullptr, "cpp-live.txt not in /tmp listing");
    // Wire-shape evidence: `dir` + a real size, not just presence.
    CHECK(found->dir == false);
    CHECK(found->size == 5);

    call("sb->files.stat(\"/tmp/cpp-live.txt\")");
    auto st = sb->files.stat("/tmp/cpp-live.txt");
    reply("size=" + std::to_string(st.size) + " dir=" + std::to_string(st.dir) +
          " modTimeMs=" + std::to_string(st.modTimeMs));
    CHECK(st.size == 5);
    CHECK(st.dir == false);
    // modTimeMs is unix-millis — guards the "modTimeMs" wire-field bug.
    CHECK(st.modTimeMs > 1600000000000LL);
  }
  {
    call("sb->files.mkdir(\"/tmp/cpp-sub\")");
    sb->files.mkdir("/tmp/cpp-sub");
    reply("ok");
    call("sb->files.rename(\"/tmp/cpp-live.txt\", \"/tmp/cpp-sub/b.txt\")");
    sb->files.rename("/tmp/cpp-live.txt", "/tmp/cpp-sub/b.txt");
    reply("ok");
    CHECK(sb->files.readText("/tmp/cpp-sub/b.txt") == "hello");
    call("sb->files.remove(\"/tmp/cpp-sub\", recursive=true)");
    sb->files.remove("/tmp/cpp-sub", true);
    reply("ok");
    auto entries = sb->files.list("/tmp");
    for (const auto& e : entries) CHECK(e.name != "cpp-sub");
  }
  {
    // Binary round-trip preserves every byte (incl. NUL and 0xFF).
    const std::string want("\x00\x01\x02\xFA\xFF", 5);
    call("sb->files.write(\"/tmp/cpp-bin\", <5 bytes>)");
    sb->files.write("/tmp/cpp-bin", want);
    reply("ok");
    call("sb->files.read(\"/tmp/cpp-bin\")");
    auto got = sb->files.read("/tmp/cpp-bin");
    reply(bytesPreview(got));
    CHECK(got.size() == want.size());
    CHECK(got == want);
  }

  // --- code.run -------------------------------------------------------------
  {
    call("sb->code.run(\"print(2**10)\")");
    auto rc = sb->code.run("print(2**10)");
    std::string text;
    for (const auto& item : rc.results)
      if (item.text) text += *item.text;
    reply("text=\"" + text + "\" charts=" + std::to_string(rc.charts.size()));
    CHECK(text.find("1024") != std::string::npos);
  }

  // --- git ------------------------------------------------------------------
  {
    const std::string repo = "/tmp/cpp-repo";
    call("sb->commands.run(\"sh\", [\"-c\", \"… git init /tmp/cpp-repo\"])");
    CommandOptions o;
    o.args = {"-c", "rm -rf " + repo + " && mkdir -p " + repo + " && cd " + repo +
                        " && git init -q && git config user.email live@solari.test"
                        " && git config user.name live"};
    auto init = sb->commands.run("sh", o);
    REQUIRE(init.exitCode == 0);
    reply("ok");

    sb->files.write(repo + "/f.txt", "v1");

    call("sb->git.status(\"/tmp/cpp-repo\")");
    auto st = sb->git.status(repo);
    reply("branch=" + st.branch + " clean=" + std::to_string(st.clean) +
          " untracked=" + std::to_string(st.untracked.size()));
    // A fresh repo with one new file: untracked, not clean.
    CHECK(st.clean == false);
    bool sawUntracked = false;
    for (const auto& u : st.untracked)
      if (u == "f.txt") sawUntracked = true;
    CHECK(sawUntracked);

    call("sb->git.add({\"f.txt\"}, \"/tmp/cpp-repo\")");
    sb->git.add({"f.txt"}, repo);
    reply("ok");

    call("sb->git.commit(\"live commit\")");
    GitCommitOptions co2;
    co2.cwd = repo;
    co2.author = "live";
    co2.email = "live@solari.test";
    auto commit = sb->git.commit("live commit", co2);
    reply(commit.hash);
    // Real evidence: a git hash is hex and >= 7 chars.
    CHECK(commit.hash.size() >= 7);
    for (char ch : commit.hash)
      REQUIRE(std::string("0123456789abcdef").find(ch) != std::string::npos);

    call("sb->git.status(\"/tmp/cpp-repo\")  (post-commit)");
    auto st2 = sb->git.status(repo);
    reply("clean=" + std::to_string(st2.clean));
    CHECK(st2.clean == true);

    call("sb->git.log({cwd, maxCount: 10})");
    GitLogOptions lo;
    lo.cwd = repo;
    lo.maxCount = 10;
    auto log = sb->git.log(lo);
    reply(std::to_string(log.size()) + " commits");
    REQUIRE(log.size() == 1);
    CHECK(log[0].message == "live commit");
    CHECK(log[0].hash == commit.hash);

    call("sb->git.branches(\"/tmp/cpp-repo\")");
    auto branches = sb->git.branches(repo);
    reply(std::to_string(branches.size()) + " branches");
    const GitBranch* current = nullptr;
    for (const auto& b : branches)
      if (b.current) current = &b;
    REQUIRE_MESSAGE(current != nullptr, "no current branch");
    CHECK(current->name == st2.branch);

    call("sb->git.checkout(\"feature\", {cwd, create: true})");
    GitCheckoutOptions ko;
    ko.cwd = repo;
    ko.create = true;
    sb->git.checkout("feature", ko);
    reply("ok");
    auto st3 = sb->git.status(repo);
    CHECK(st3.branch == "feature");
  }

  // --- query / re-attach ----------------------------------------------------
  {
    call("client.get(id)");
    auto view = client.get(sb->id());
    reply("kind=" + view.kind + " state=" + view.state);
    CHECK(view.sandboxId == sb->id());
    CHECK(view.kind == "desktop");
    CHECK(view.state == "running");
    // By design (toSandboxView, router.ts:2501) the view carries NO
    // controlUrl/streamUrl — connect() derives them. Assert the design.
    CHECK(view.controlUrl.has_value() == false);
  }
  {
    call("client.connect(id)");
    auto re = client.connect(sb->id());
    std::cout << "<- kind=" << re->kind()
              << " streamUrl=" << (re->streamUrl() ? *re->streamUrl() : "(none)")
              << std::endl;
    CHECK(re->id() == sb->id());
    CHECK(re->kind() == "desktop");
    // GET carries no streamUrl, so connect() DERIVES it (desktops only).
    REQUIRE_MESSAGE(re->streamUrl().has_value(),
                    "connect derived no streamUrl for a desktop");
    const std::string suffix = "/stream/" + re->id();
    CHECK(re->streamUrl()->size() >= suffix.size());
    CHECK(re->streamUrl()->compare(re->streamUrl()->size() - suffix.size(),
                                   suffix.size(), suffix) == 0);
    CHECK((re->streamUrl()->rfind("ws://", 0) == 0 ||
           re->streamUrl()->rfind("wss://", 0) == 0));

    // The re-attached handle is live, not just a struct.
    re->connect();
    call("reattached.commands.run(\"sh\", [\"-c\",\"echo reattached\"])");
    CommandOptions o;
    o.args = {"-c", "echo reattached"};
    auto r = re->commands.run("sh", o);
    reply("stdout=" + r.stdout_);
    CHECK(r.stdout_.find("reattached") != std::string::npos);
    re->close();
  }
}

// ---------------------------------------------------------------------------
// Sandbox — the other half of the streamUrl design.
// ---------------------------------------------------------------------------
TEST_CASE("live: headless sandbox has no stream URL") {
  if (!liveEnabled()) {
    MESSAGE("skipping: set SOLARI_LIVE=1 (and SOLARI_API_KEY) to run live tests");
    return;
  }
  const std::string key = envOr("SOLARI_API_KEY");
  REQUIRE_MESSAGE(!key.empty(), "SOLARI_LIVE=1 but SOLARI_API_KEY is empty");

  ClientOptions co;
  co.apiKey = key;
  co.baseUrl = envOr("SOLARI_BASE_URL", kDefaultBaseUrl);
  Client client(co);

  auto sb = createWithRetry(client, /*desktop=*/false);
  REQUIRE(sb != nullptr);
  VmGuard guard{client, sb->id()};

  CHECK(sb->kind() == "sandbox");
  // The whole point: no display => no stream URL on create...
  CHECK(sb->streamUrl().has_value() == false);

  // ...and none derived on connect either.
  call("client.connect(id)");
  auto re = client.connect(sb->id());
  std::cout << "<- kind=" << re->kind()
            << " streamUrl=" << (re->streamUrl() ? *re->streamUrl() : "(none)")
            << std::endl;
  CHECK(re->streamUrl().has_value() == false);

  // It is a real, working sandbox all the same.
  call("sb->connect()");
  sb->connect();
  reply("ok");
  call("sb->commands.run(\"uname\", [\"-s\"])");
  CommandOptions o;
  o.args = {"-s"};
  auto r = sb->commands.run("uname", o);
  reply("stdout=" + r.stdout_);
  CHECK(trim(r.stdout_) == "Linux");
}

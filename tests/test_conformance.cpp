// Conformance runner — C++ side.
//
// Runs every row of tests/conformance/spec/{wire,errors,retry}.json (the Solari SDK conformance suite, pinned in
// tests/conformance/SOURCE.txt) against THIS SDK through the shared mock gateway, `node tests/conformance/mock-server.mjs`. The rows are
// the same ones the TypeScript runner and the other bindings run, so a rule one binding got wrong is a red row here.
//
//  * An op this binding does not have is listed in tests/conformance/unsupported.json with the reason; an unlisted gap fails.
//  * A row this binding currently fails because the BINDING is wrong is listed in tests/conformance/knownGaps.json with what closes it;
//    the runner asserts it STILL fails, so a gap cannot decay into a skip and closing it without deleting the entry fails too.
//  * Rows run concurrently on a small pool of mock processes (the retry rows wait out the SDK's real backoff).
//
// Needs `node` on PATH (the mock and dump-rows.mjs are JavaScript). SOLARI_CONFORMANCE_DIR is injected by CMake.

#include <doctest/doctest.h>

#include <curl/curl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include "solari/solari.hpp"

extern char** environ;

using namespace solari;
using nlohmann::json;

namespace {

const std::string kDir = SOLARI_CONFORMANCE_DIR;

std::string readFile(const std::string& path) {
  std::ifstream f(path);
  REQUIRE_MESSAGE(f.good(), "cannot read " << path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string runAndRead(const std::string& cmd) {
  FILE* p = popen(cmd.c_str(), "r");
  REQUIRE_MESSAGE(p != nullptr, "cannot run " << cmd);
  std::string out;
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
  REQUIRE_MESSAGE(pclose(p) == 0, cmd << " failed");
  return out;
}

// --- the mock gateway as a child process ------------------------------------

struct Mock {
  pid_t pid = -1;
  std::string url;

  Mock() {
    int fds[2];
    REQUIRE(pipe(fds) == 0);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    std::string script = kDir + "/mock-server.mjs";
    char* argv[] = {const_cast<char*>("node"), const_cast<char*>(script.c_str()), const_cast<char*>("--port"), const_cast<char*>("0"), nullptr};
    int rc = posix_spawnp(&pid, "node", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    REQUIRE_MESSAGE(rc == 0, "cannot start node (is it on PATH?)");
    std::string line;
    char c;
    while (read(fds[0], &c, 1) == 1 && c != '\n') line += c;
    close(fds[0]);
    url = json::parse(line).at("url").get<std::string>();
  }
  ~Mock() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      int st;
      waitpid(pid, &st, 0);
    }
  }
  Mock(const Mock&) = delete;
  Mock& operator=(const Mock&) = delete;

  static size_t sink(char* p, size_t s, size_t n, void* out) {
    static_cast<std::string*>(out)->append(p, s * n);
    return s * n;
  }
  json control(const std::string& method, const std::string& path, const std::string& body = "") {
    CURL* c = curl_easy_init();
    std::string out;
    curl_slist* h = curl_slist_append(nullptr, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_URL, (url + path).c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (method == "POST") curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
    CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK) throw std::runtime_error(std::string("mock control call failed: ") + curl_easy_strerror(rc));
    return json::parse(out);
  }
};

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
  for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
  return s;
}

// --- comparing: expected is a deep SUBSET of actual --------------------------

std::string subsetMismatch(const json& exp, const json& act, const std::string& path) {
  if (exp.is_object()) {
    if (!act.is_object()) return path + ": expected object, got " + act.dump();
    for (auto it = exp.begin(); it != exp.end(); ++it) {
      if (!act.contains(it.key())) return path + "." + it.key() + ": missing (expected " + it.value().dump() + ")";
      std::string m = subsetMismatch(it.value(), act[it.key()], path + "." + it.key());
      if (!m.empty()) return m;
    }
    return "";
  }
  if (exp.is_array()) {
    if (!act.is_array() || act.size() != exp.size()) return path + ": expected array of " + std::to_string(exp.size()) + ", got " + act.dump();
    for (size_t i = 0; i < exp.size(); i++) {
      std::string m = subsetMismatch(exp[i], act[i], path + "[" + std::to_string(i) + "]");
      if (!m.empty()) return m;
    }
    return "";
  }
  return exp == act ? "" : path + ": expected " + exp.dump() + ", got " + act.dump();
}

// --- the adapter: abstract op -> this SDK; its errors -> {type,status,code,retryable} --

json normalizeError(const std::exception& e) {
  json out = {{"type", "other:" + std::string(typeid(e).name())}, {"message", e.what()}};
  if (auto* g = dynamic_cast<const GatewayError*>(&e)) {
    out["type"] = "gateway";
    if (auto* ns = dynamic_cast<const SandboxNotStoppedError*>(g)) {
      out["type"] = "not_stopped";
      if (ns->state) out["state"] = *ns->state;
    } else if (dynamic_cast<const AuthError*>(g)) out["type"] = "auth";
    else if (dynamic_cast<const PlanError*>(g)) out["type"] = "plan";
    else if (dynamic_cast<const ConcurrencyLimitError*>(g)) out["type"] = "concurrency_limit";
    else if (dynamic_cast<const NoCapacityError*>(g)) out["type"] = "no_capacity";
    out["status"] = g->status;
    if (g->code) out["code"] = *g->code;
    if (g->body.is_object() && g->body.contains("retryable")) out["retryable"] = g->body["retryable"];
  }
  if (dynamic_cast<const ValidationError*>(&e)) out["type"] = "validation";
  if (auto* a = dynamic_cast<const ActionError*>(&e)) {
    out["type"] = "action";
    if (a->code) out["code"] = *a->code;
  } else if (dynamic_cast<const ConnectionError*>(&e)) {
    out["type"] = "connection";
  }
  return out;
}

template <typename T>
void setIf(const json& in, const char* key, std::optional<T>& out) {
  if (in.contains(key)) out = in[key].get<T>();
}

CreateSandboxOptions createOptions(const json& in) {
  CreateSandboxOptions o;
  setIf(in, "template", o.template_);
  setIf(in, "cpu", o.cpu);
  setIf(in, "memMb", o.memMb);
  setIf(in, "diskGb", o.diskGb);
  setIf(in, "timeoutMs", o.timeoutMs);
  setIf(in, "ttlSeconds", o.ttlSeconds);
  setIf(in, "fromSnapshot", o.fromSnapshot);
  setIf(in, "isolation", o.isolation);
  setIf(in, "resolution", o.resolution);
  setIf(in, "record", o.record);
  if (in.contains("envs")) o.envs = in["envs"].get<std::map<std::string, std::string>>();
  if (in.contains("metadata")) o.metadata = in["metadata"].get<std::map<std::string, std::string>>();
  if (in.contains("lifecycle")) {
    const json& l = in["lifecycle"];
    Lifecycle lc;
    lc.onTimeout = l.value("onTimeout", "");
    setIf(l, "autoResume", lc.autoResume);
    setIf(l, "parkAfterMs", lc.parkAfterMs);
    setIf(l, "parkMaxMs", lc.parkMaxMs);
    setIf(l, "hibernateAfterParkedMs", lc.hibernateAfterParkedMs);
    setIf(l, "checkpointEveryMs", lc.checkpointEveryMs);
    o.lifecycle = lc;
  }
  if (in.contains("volumes")) for (const auto& v : in["volumes"]) o.volumes.push_back({v["volumeId"].get<std::string>(), v["path"].get<std::string>()});
  return o;
}

json handleView(const Sandbox& s) {
  json v = {{"sandboxId", s.id()}, {"kind", s.kind()}};
  if (s.streamUrl()) v["streamUrl"] = *s.streamUrl();
  if (s.recordingUrl()) v["recordingUrl"] = *s.recordingUrl();
  return v;
}

const std::set<std::string>& supportedOps() {
  static const std::set<std::string> ops = {"git.status", "git.branches", "git.log", "commands.run", "commands.runCancellable", "sandbox.create", "sandbox.createDesktop", "sandbox.get", "sandbox.pause", "sandbox.resume",
                                            "sandbox.start", "sandbox.kill", "sandbox.connect"};
  return ops;
}

/** Run one op against `baseUrl`. Returns { result } or { error }; never throws. A handle op first creates a sandbox (the row's mock lists that exchange). */
json runOp(const std::string& op, const json& input, const std::string& baseUrl) {
  try {
    ClientOptions co;
    co.apiKey = "conformance-test-key";
    co.baseUrl = baseUrl;
    Client c(co);
    CreateSandboxOptions base;
    base.template_ = "base";
    if (op == "sandbox.create") return {{"result", handleView(*c.create(createOptions(input)))}};
    if (op == "sandbox.createDesktop") return {{"result", handleView(*c.createDesktop(createOptions(input)))}};
    if (op == "sandbox.connect") return {{"result", handleView(*c.connect(input.at("id")))}};
    if (op == "sandbox.get") {
      SandboxView v = c.get(input.at("id"));
      json r = {{"sandboxId", v.sandboxId}, {"kind", v.kind}, {"state", v.state}};
      if (v.controlUrl) r["controlUrl"] = *v.controlUrl;
      return {{"result", r}};
    }
    if (op == "git.status" || op == "git.branches" || op == "git.log") {
      auto s = c.create(base);
      std::optional<std::string> cwd;
      if (input.contains("cwd")) cwd = input["cwd"].get<std::string>();
      if (op == "git.status") {
        GitStatus st = s->git.status(cwd);
        json entries = json::array();
        for (const auto& e : st.entries) {
          json v = {{"path", e.path}, {"index", std::string(1, e.index)}, {"worktree", std::string(1, e.worktree)}};
          if (e.origPath) v["origPath"] = *e.origPath;
          entries.push_back(v);
        }
        json r = {{"branch", st.branch}, {"detached", st.detached}, {"ahead", st.ahead}, {"behind", st.behind}, {"staged", st.staged},
                  {"modified", st.modified}, {"untracked", st.untracked}, {"conflicted", st.conflicted}, {"clean", st.clean}, {"entries", entries}};
        if (st.upstream) r["upstream"] = *st.upstream;
        return {{"result", r}};
      }
      if (op == "git.branches") {
        json list = json::array();
        for (const auto& b : s->git.branches(cwd)) {
          json v = {{"name", b.name}, {"commit", b.commit}, {"current", b.current}};
          if (b.upstream) v["upstream"] = *b.upstream;
          list.push_back(v);
        }
        return {{"result", {{"list", list}}}};
      }
      GitLogOptions lo;
      lo.cwd = cwd;
      if (input.contains("maxCount")) lo.maxCount = input["maxCount"].get<int>();
      json list = json::array();
      for (const auto& cm : s->git.log(lo))
        list.push_back({{"hash", cm.hash}, {"author", cm.author}, {"email", cm.email}, {"date", cm.date}, {"message", cm.message}, {"body", cm.body}});
      return {{"result", {{"list", list}}}};
    }
    if (op == "commands.run" || op == "commands.runCancellable") {
      auto s = c.create(base);
      CommandOptions o;
      if (input.contains("args")) o.args = input["args"].get<std::vector<std::string>>();
      if (input.contains("timeoutMs")) o.timeoutMs = input["timeoutMs"].get<long>();
      // via "ws": a non-empty env forces the live control connection; via "rest": nothing does (no channel, no env).
      if (input.value("via", "rest") == "ws") o.env = {{"SOLARI_CONFORMANCE", "1"}};
      if (input.contains("user")) o.user = input["user"].get<std::string>();
      // A call the caller can cancel (a cancel token) must run on the live connection: aborting a one-shot /exec leaves the process.
      if (op == "commands.runCancellable") o.cancel = std::make_shared<CancelToken>();
      CommandResult r = s->commands.run(input["cmd"].get<std::string>(), o);
      return {{"result", {{"exitCode", r.exitCode}, {"stdout", r.stdout_}, {"stderr", r.stderr_}, {"timedOut", r.timedOut}, {"oomKilled", r.oomKilled}, {"stdoutBase64", base64Encode(r.stdoutBytes)}}}};
    }
    auto s = c.create(base);
    if (op == "sandbox.pause") s->pause();
    else if (op == "sandbox.resume") s->resume();
    else if (op == "sandbox.start") s->start();
    else if (op == "sandbox.kill") s->kill();
    else return {{"error", {{"type", "other:unmapped-op"}, {"message", op}}}};
    return {{"result", json::object()}};
  } catch (const std::exception& e) {
    return {{"error", normalizeError(e)}};
  }
}

/** Run one row on a mock; returns "" when it passes, else why it fails. */
std::string checkRow(Mock& mock, const json& row) {
  mock.control("POST", "/__reset");
  std::string mockJson = replaceAll(replaceAll(row.value("mock", json::array()).dump(), "$MOCK_WS", replaceAll(mock.url, "http", "ws")), "$MOCK_URL", mock.url);
  const bool strict = row.contains("expect") && row["expect"].contains("wsCalls");
  json armed = mock.control("POST", "/__arm", json{{"mock", json::parse(mockJson)}, {"ws", row.value("ws", json::array())}, {"strict", strict}}.dump());
  if (!armed.value("ok", false)) return "could not arm the mock: " + armed.dump();
  json out = runOp(row["op"], row.value("input", json::object()), mock.url);
  json log = mock.control("GET", "/__log");
  int requests = 0;
  for (const auto& r : log["requests"]) if (!r.contains("upgrade")) requests++;
  if (!log["mismatches"].empty()) {
    std::string why = "request did not fit the script:";
    for (const auto& m : log["mismatches"]) why += " " + m.value("request", "") + ": " + m.value("reason", "") + ";";
    return why;
  }
  json exp = row.value("expect", json::object());
  if (exp.contains("error")) {
    if (!out.contains("error")) return "expected error " + exp["error"].dump() + ", got result " + out["result"].dump();
    std::string m = subsetMismatch(exp["error"], out["error"], "error");
    if (!m.empty()) return m + " (message: " + out["error"].value("message", "") + ")";
  } else {
    if (out.contains("error")) return "expected success, got error " + out["error"].dump();
    if (exp.contains("result")) {
      std::string m = subsetMismatch(exp["result"], out["result"], "result");
      if (!m.empty()) return m;
    }
  }
  if (exp.contains("requests") && requests != exp["requests"].get<int>())
    return "expected " + std::to_string(exp["requests"].get<int>()) + " request(s), made " + std::to_string(requests);
  if (exp.contains("wsCalls") && static_cast<int>(log["wsCalls"].size()) != exp["wsCalls"].get<int>())
    return "expected " + std::to_string(exp["wsCalls"].get<int>()) + " control call(s), made " + std::to_string(log["wsCalls"].size());
  if (log["remaining"].get<int>() != 0) return std::to_string(log["remaining"].get<int>()) + " scripted exchange(s) never happened";
  return "";
}

}  // namespace

TEST_CASE("conformance rows run through the shared mock gateway") {
  json rows = json::parse(runAndRead("cd '" + kDir + "' && node dump-rows.mjs"));
  std::map<std::string, std::string> unsupported;
  for (const auto& u : json::parse(readFile(kDir + "/unsupported.json"))) unsupported[u["op"]] = u["why"];
  std::map<std::string, json> gaps;
  for (const auto& g : json::parse(readFile(kDir + "/knownGaps.json"))) gaps[g["id"]] = g;

  std::vector<std::string> failures(rows.size());
  std::vector<char> ran(rows.size(), 0);  // not vector<bool>: workers write distinct elements concurrently
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (int w = 0; w < 8; w++) {
    pool.emplace_back([&] {
      Mock mock;
      for (size_t i; (i = next++) < rows.size();) {
        if (!supportedOps().count(rows[i]["op"])) continue;
        ran[i] = true;
        try { failures[i] = checkRow(mock, rows[i]); } catch (const std::exception& e) { failures[i] = std::string("runner error: ") + e.what(); }
      }
    });
  }
  for (auto& t : pool) t.join();

  size_t passed = 0, skipped = 0, gapRows = 0;
  std::set<std::string> keys, ops;
  for (size_t i = 0; i < rows.size(); i++) {
    std::string key = rows[i]["key"], op = rows[i]["op"];
    keys.insert(key);
    ops.insert(op);
    if (!ran[i]) {
      CHECK_MESSAGE(unsupported.count(op) == 1, op << " is not implemented by the C++ adapter and not listed in unsupported.json");
      skipped++;
    } else if (gaps.count(key)) {
      CHECK_MESSAGE(!failures[i].empty(), key << " is listed in knownGaps.json (" << gaps[key].value("why", "") << ") but now PASSES: delete the entry");
      gapRows++;
    } else {
      CHECK_MESSAGE(failures[i].empty(), key << ": " << failures[i]);
      if (failures[i].empty()) passed++;
    }
  }
  for (const auto& [id, g] : gaps) CHECK_MESSAGE(keys.count(id) == 1, "knownGaps entry " << id << " matches no row");
  for (const auto& [op, why] : unsupported) CHECK_MESSAGE(ops.count(op) == 1, "unsupported entry " << op << " matches no row");
  MESSAGE("conformance: " << rows.size() << " rows, " << passed << " pass, " << gapRows << " known gaps, " << skipped << " rows of " << unsupported.size()
                          << " unsupported ops; supported ops " << supportedOps().size() << " of " << ops.size());
}

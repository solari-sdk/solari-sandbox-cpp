// Real git against real HTTP servers: the credentials reach git through the environment and the origin-bound inline
// credential helper, and nothing about them is left in .git/config or argv; a redirect to another host gets nothing.
// Skipped where git or git-http-backend is absent.
#include <doctest/doctest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "solari/solari.hpp"

using namespace solari;

namespace {

const char* kUser = "alice";
const char* kPass = "p@ss w0rd/x";

std::string backendPath() {
  for (const char* p : {"/usr/lib/git-core/git-http-backend", "/usr/libexec/git-core/git-http-backend"})
    if (access(p, X_OK) == 0) return p;
  return "";
}

bool haveGit() { return std::system("git --version >/dev/null 2>&1") == 0; }

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Runs a command with fork/exec: optional cwd, extra env (on a clean environment), optional stdin; captures stdout.
struct Proc {
  int code = -1;
  std::string out, err;
};

Proc runProc(const std::vector<std::string>& argv, const std::string& cwd,
             const std::map<std::string, std::string>& env, const std::string& input = "") {
  int in[2], out[2], er[2];
  if (pipe(in) || pipe(out) || pipe(er)) return {};
  pid_t pid = fork();
  if (pid == 0) {
    dup2(in[0], 0);
    dup2(out[1], 1);
    dup2(er[1], 2);
    for (int fd : {in[0], in[1], out[0], out[1], er[0], er[1]}) close(fd);
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    clearenv();
    for (const auto& kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  close(in[0]);
  close(out[1]);
  close(er[1]);
  std::thread w([&] {
    if (!input.empty()) {
      size_t off = 0;
      while (off < input.size()) {
        ssize_t n = write(in[1], input.data() + off, input.size() - off);
        if (n <= 0) break;
        off += static_cast<size_t>(n);
      }
    }
    close(in[1]);
  });
  Proc p;
  std::thread e([&] {
    char buf[4096];
    ssize_t n;
    while ((n = read(er[0], buf, sizeof buf)) > 0) p.err.append(buf, static_cast<size_t>(n));
  });
  char buf[4096];
  ssize_t n;
  while ((n = read(out[0], buf, sizeof buf)) > 0) p.out.append(buf, static_cast<size_t>(n));
  w.join();
  e.join();
  close(out[0]);
  close(er[0]);
  int st = 0;
  waitpid(pid, &st, 0);
  p.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return p;
}

std::map<std::string, std::string> baseEnv() {
  const char* path = std::getenv("PATH");
  return {{"PATH", path ? path : "/usr/bin:/bin"}};
}

std::string sh(const std::vector<std::string>& argv) {
  Proc p = runProc(argv, "", baseEnv());
  REQUIRE_MESSAGE(p.code == 0, argv[0] << " failed: " << p.err);
  return p.out;
}

// ---- tiny HTTP servers ------------------------------------------------------
struct Request {
  std::string method, target, body;
  std::map<std::string, std::string> headers;
};

bool readRequest(int fd, Request& r) {
  std::string buf;
  char tmp[4096];
  size_t end;
  while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    buf.append(tmp, static_cast<size_t>(n));
  }
  std::istringstream hs(buf.substr(0, end));
  std::string line;
  std::getline(hs, line);
  std::istringstream first(line);
  first >> r.method >> r.target;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    auto c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = line.substr(0, c), v = line.substr(c + 1);
    for (auto& ch : k) ch = static_cast<char>(tolower(ch));
    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
    r.headers[k] = v;
  }
  r.body = buf.substr(end + 4);
  size_t want = r.headers.count("content-length") ? std::stoul(r.headers["content-length"]) : 0;
  while (r.body.size() < want) {
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) break;
    r.body.append(tmp, static_cast<size_t>(n));
  }
  return true;
}

void respond(int fd, const std::string& status, const std::vector<std::pair<std::string, std::string>>& headers,
             const std::string& body) {
  std::string out = "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n";
  for (const auto& h : headers) out += h.first + ": " + h.second + "\r\n";
  out += "\r\n" + body;
  size_t off = 0;
  while (off < out.size()) {
    ssize_t n = send(fd, out.data() + off, out.size() - off, 0);
    if (n <= 0) break;
    off += static_cast<size_t>(n);
  }
}

class Server {
 public:
  using Handler = std::function<void(int, const Request&)>;
  explicit Server(Handler h) : handler_(std::move(h)) {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a);
    listen(fd_, 16);
    socklen_t len = sizeof a;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    th_ = std::thread([this] {
      while (!stop_) {
        int c = accept(fd_, nullptr, nullptr);
        if (c < 0) break;
        Request r;
        if (readRequest(c, r)) handler_(c, r);
        close(c);
      }
    });
  }
  ~Server() {
    stop_ = true;
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    if (th_.joinable()) th_.join();
  }
  int port() const { return port_; }

 private:
  Handler handler_;
  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread th_;
};

// GitOps whose runner executes the real git (clean HOME), recording every argv.
struct RealGit {
  std::string home;
  std::vector<std::vector<std::string>> argvs;
  std::mutex mu;
  CommandResult run(const std::string& cmd, const CommandOptions& o) {
    {
      std::lock_guard<std::mutex> l(mu);
      argvs.push_back(o.args);
    }
    std::vector<std::string> argv = {cmd};
    argv.insert(argv.end(), o.args.begin(), o.args.end());
    auto env = baseEnv();
    env["HOME"] = home;
    env["GIT_CONFIG_NOSYSTEM"] = "1";
    for (const auto& kv : o.env) env[kv.first] = kv.second;
    Proc p = runProc(argv, o.cwd.value_or(""), env);
    return {p.code, p.out, p.err};
  }
};

std::string tempDir(const std::string& tag) {
  std::string d = "/tmp/solari-gitreal-cpp-" + tag + "-" + std::to_string(getpid());
  std::system(("rm -rf '" + d + "' && mkdir -p '" + d + "'").c_str());
  return d;
}

}  // namespace

TEST_CASE("real git: private clone and push use env + origin-bound helper; nothing in .git/config or argv") {
  std::string be = backendPath();
  if (be.empty() || !haveGit()) return;
  std::string dir = tempDir("priv");
  std::string root = dir + "/srv", bare = root + "/repo.git", seed = dir + "/seed";
  sh({"git", "init", "-q", "--bare", "-b", "main", bare});
  sh({"git", "-C", bare, "config", "http.receivepack", "true"});
  sh({"git", "init", "-q", "-b", "main", seed});
  sh({"git", "-C", seed, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "first"});
  sh({"git", "-C", seed, "push", "-q", bare, "main"});

  std::string want = "Basic " + base64Encode(std::string(kUser) + ":" + kPass);
  std::mutex mu;
  std::vector<std::string> seen;
  Server srv([&](int fd, const Request& r) {
    std::string auth = r.headers.count("authorization") ? r.headers.at("authorization") : "";
    {
      std::lock_guard<std::mutex> l(mu);
      seen.push_back(auth);
    }
    if (auth != want) {
      respond(fd, "401 Unauthorized", {{"WWW-Authenticate", "Basic realm=\"git\""}}, "");
      return;
    }
    auto q = r.target.find('?');
    std::map<std::string, std::string> env = baseEnv();
    env["GIT_PROJECT_ROOT"] = root;
    env["GIT_HTTP_EXPORT_ALL"] = "1";
    env["REQUEST_METHOD"] = r.method;
    env["PATH_INFO"] = r.target.substr(0, q);
    env["QUERY_STRING"] = q == std::string::npos ? "" : r.target.substr(q + 1);
    env["CONTENT_TYPE"] = r.headers.count("content-type") ? r.headers.at("content-type") : "";
    env["CONTENT_LENGTH"] = std::to_string(r.body.size());
    env["REMOTE_USER"] = kUser;
    env["REMOTE_ADDR"] = "127.0.0.1";
    env["HTTP_CONTENT_ENCODING"] = r.headers.count("content-encoding") ? r.headers.at("content-encoding") : "";
    Proc p = runProc({be}, "", env, r.body);
    auto sep = p.out.find("\r\n\r\n");
    if (sep == std::string::npos) sep = 0;
    std::string status = "200 OK";
    std::vector<std::pair<std::string, std::string>> hs;
    std::istringstream head(p.out.substr(0, sep));
    std::string line;
    while (std::getline(head, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      auto c = line.find(':');
      if (c == std::string::npos) continue;
      std::string k = line.substr(0, c), v = line.substr(c + 1);
      while (!v.empty() && v.front() == ' ') v.erase(v.begin());
      if (strcasecmp(k.c_str(), "status") == 0) status = v;
      else if (strcasecmp(k.c_str(), "content-length") != 0) hs.push_back({k, v});
    }
    respond(fd, status, hs, p.out.substr(std::min(sep + 4, p.out.size())));
  });

  std::string home = dir + "/home";
  std::system(("mkdir -p '" + home + "'").c_str());
  RealGit rg{home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  std::string plain = "http://127.0.0.1:" + std::to_string(srv.port()) + "/repo.git";

  GitCloneOptions nocreds;
  nocreds.path = dir + "/nocreds";
  CHECK_THROWS_AS(git.clone(plain, nocreds), Error);

  GitCloneOptions bad;
  bad.path = dir + "/bad";
  bad.username = kUser;
  bad.password = std::string("wrong-") + kPass;
  try {
    git.clone(plain, bad);
    FAIL("clone with a wrong password should fail");
  } catch (const Error& e) {
    CHECK(std::string(e.what()).find(*bad.password) == std::string::npos);
  }

  GitCloneOptions good;
  good.path = dir + "/work";
  good.username = kUser;
  good.password = kPass;
  git.clone(plain, good);
  {
    std::lock_guard<std::mutex> l(mu);
    CHECK(std::find(seen.begin(), seen.end(), want) != seen.end());
  }
  std::string cfg = slurp(dir + "/work/.git/config");
  CHECK(cfg.find("url = " + plain) != std::string::npos);
  for (const std::string& needle : {std::string(kPass), std::string("p%40ss"), std::string(kUser) + ":", std::string("credential")})
    CHECK_MESSAGE(cfg.find(needle) == std::string::npos, ".git/config contains " << needle << ":\n" << cfg);
  CHECK(access((home + "/.git-credentials").c_str(), F_OK) != 0);

  sh({"git", "-C", dir + "/work", "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "--allow-empty", "-m", "second"});
  GitRemoteOptions po;
  po.cwd = dir + "/work";
  po.username = kUser;
  po.password = kPass;
  po.branch = "main";
  git.push(po);
  CHECK(sh({"git", "-C", bare, "log", "--format=%s", "main"}).find("second") != std::string::npos);
  CHECK(slurp(dir + "/work/.git/config").find(kPass) == std::string::npos);
  for (const auto& a : rg.argvs)
    for (const auto& s : a) {
      CHECK(s.find(kPass) == std::string::npos);
      CHECK(s.find("p%40ss") == std::string::npos);
    }
  std::system(("rm -rf '" + dir + "'").c_str());
}

TEST_CASE("real git: a redirect to another host never receives the password; --upload-pack is inert") {
  if (backendPath().empty() || !haveGit()) return;
  std::string dir = tempDir("redir");
  std::mutex mu;
  std::vector<std::string> bSeen;
  Server b([&](int fd, const Request& r) {
    {
      std::lock_guard<std::mutex> l(mu);
      bSeen.push_back(r.headers.count("authorization") ? r.headers.at("authorization") : "");
    }
    respond(fd, "401 Unauthorized", {{"WWW-Authenticate", "Basic realm=\"other\""}}, "");
  });
  int bPort = b.port();
  Server a([&](int fd, const Request& r) {
    respond(fd, "302 Found", {{"Location", "http://localhost:" + std::to_string(bPort) + r.target}}, "");
  });
  RealGit rg{dir, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  GitCloneOptions o;
  o.path = dir + "/x";
  o.username = kUser;
  o.password = kPass;
  CHECK_THROWS_AS(git.clone("http://127.0.0.1:" + std::to_string(a.port()) + "/repo.git", o), Error);
  {
    std::lock_guard<std::mutex> l(mu);
    REQUIRE_MESSAGE(!bSeen.empty(), "git did not follow the redirect, so the test proves nothing");
    for (const auto& h : bSeen) CHECK_MESSAGE(h.empty(), "the redirect target received Authorization: " << h);
  }
  std::string mark = dir + "/pwned";
  GitCloneOptions hostile;
  hostile.path = dir + "/y";
  CHECK_THROWS_AS(git.clone("--upload-pack=touch " + mark, hostile), Error);
  CHECK(access(mark.c_str(), F_OK) != 0);
  std::system(("rm -rf '" + dir + "'").c_str());
}

// ---- status / branches / log against REAL git (no stub output) -------------------------------------------------------------
// The parsers used to lose information: quoted paths, a rename as one "old -> new" string, an untracked directory as "dir/", a
// conflict listed as staged AND modified, a detached HEAD listed as a branch, log on a repository with no commit failing, and the
// message body dropped.
namespace {

struct ParseRepo {
  std::string dir, repo, home;
  ParseRepo(const std::string& tag) {
    dir = tempDir(tag);
    repo = dir + "/repo";
    home = dir + "/home";
    std::system(("mkdir -p '" + repo + "' '" + home + "'").c_str());
    g({"init", "-q", "-b", "main"});
    g({"config", "user.name", "T"});
    g({"config", "user.email", "t@e.x"});
  }
  std::string g(const std::vector<std::string>& args) {
    std::vector<std::string> argv = {"git"};
    argv.insert(argv.end(), args.begin(), args.end());
    auto env = baseEnv();
    env["GIT_CONFIG_GLOBAL"] = "/dev/null";
    env["GIT_CONFIG_SYSTEM"] = "/dev/null";
    return runProc(argv, repo, env).out;  // exit status ignored: a merge conflict is the point of one test
  }
  void write(const std::string& name, const std::string& body) {
    std::string p = repo + "/" + name;
    auto slash = p.rfind('/');
    std::system(("mkdir -p '" + p.substr(0, slash) + "'").c_str());
    std::ofstream(p, std::ios::binary) << body;
  }
};

std::vector<std::string> sortedCopy(std::vector<std::string> v) {
  std::sort(v.begin(), v.end());
  return v;
}

}  // namespace

TEST_CASE("real git: an empty repository: log is empty, branches empty, status clean on the unborn branch") {
  if (!haveGit()) return;
  ParseRepo r("empty");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  CHECK(git.log(GitLogOptions{r.repo, std::nullopt}).empty());
  CHECK(git.branches(r.repo).empty());
  GitStatus st = git.status(r.repo);
  CHECK(st.branch == "main");
  CHECK(st.clean);
  CHECK(st.entries.empty());
}

TEST_CASE("real git: status paths are verbatim and an untracked directory lists its files") {
  if (!haveGit()) return;
  ParseRepo r("paths");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  for (auto [n, b] : std::vector<std::pair<std::string, std::string>>{{"a b.txt", "1"}, {"\xC3\xA9 \xC3\xBC.txt", "2"}, {"tab\there", "3"}, {"nd/inner", "n"}, {"nd/deep file", "n"}})
    r.write(n, b);
  GitStatus st = git.status(r.repo);
  CHECK(sortedCopy(st.untracked) == sortedCopy({"a b.txt", "\xC3\xA9 \xC3\xBC.txt", "tab\there", "nd/inner", "nd/deep file"}));
}

TEST_CASE("real git: a rename is the new path with the old path in entries; a deletion is staged; staged and modified stay separate") {
  if (!haveGit()) return;
  ParseRepo r("rename");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  r.write("plain", "2");
  r.write("gone me", "1");
  r.g({"add", "-A"});
  r.g({"commit", "-q", "-m", "first"});
  r.write("plain", "2\nmore");
  r.g({"mv", "plain", "renamed file"});
  r.write("staged", "s");
  r.g({"add", "staged"});
  r.g({"rm", "-q", "--cached", "gone me"});
  GitStatus st = git.status(r.repo);
  CHECK(sortedCopy(st.staged) == sortedCopy({"gone me", "renamed file", "staged"}));
  CHECK(st.modified == std::vector<std::string>{"renamed file"});  // RM: the rename is staged and the content changed again
  const GitStatusEntry* ren = nullptr;
  for (const auto& e : st.entries) if (e.path == "renamed file") ren = &e;
  REQUIRE(ren != nullptr);
  CHECK(ren->origPath.value_or("") == "plain");
  CHECK(ren->index == 'R');
  CHECK(ren->worktree == 'M');
  for (const auto& p : st.staged) CHECK(p.find("->") == std::string::npos);
}

TEST_CASE("real git: a merge conflict is listed once, as conflicted") {
  if (!haveGit()) return;
  ParseRepo r("conflict");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  r.write("base", "0");
  r.g({"add", "-A"});
  r.g({"commit", "-q", "-m", "base"});
  r.g({"checkout", "-q", "-b", "c1"});
  r.write("conflict", "A");
  r.g({"add", "conflict"});
  r.g({"commit", "-q", "-m", "c1"});
  r.g({"checkout", "-q", "main"});
  r.write("conflict", "B");
  r.g({"add", "conflict"});
  r.g({"commit", "-q", "-m", "c2"});
  r.g({"merge", "c1"});
  GitStatus st = git.status(r.repo);
  CHECK(st.conflicted == std::vector<std::string>{"conflict"});
  CHECK(st.staged.empty());
  CHECK(st.modified.empty());
  CHECK_FALSE(st.clean);
}

TEST_CASE("real git: upstream with ahead/behind, a detached HEAD, and branches that are real branches only") {
  if (!haveGit()) return;
  ParseRepo r("upstream");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  r.write("f", "1");
  r.g({"add", "-A"});
  r.g({"commit", "-q", "-m", "one"});
  r.g({"branch", "feat/x"});
  r.g({"branch", "topic"});
  r.g({"remote", "add", "origin", r.repo});
  r.g({"fetch", "-q", "origin"});
  r.g({"branch", "--set-upstream-to=origin/main", "main"});
  r.write("f", "2");
  r.g({"commit", "-qam", "two"});
  GitStatus st = git.status(r.repo);
  CHECK(st.branch == "main");
  CHECK(st.upstream.value_or("") == "origin/main");
  CHECK(st.ahead == 1);
  CHECK(st.behind == 0);
  auto bs = git.branches(r.repo);
  std::vector<std::string> names;
  const GitBranch* mainB = nullptr;
  for (const auto& b : bs) {
    names.push_back(b.name);
    if (b.name == "main") mainB = &b;
  }
  CHECK(sortedCopy(names) == sortedCopy({"feat/x", "main", "topic"}));
  REQUIRE(mainB != nullptr);
  CHECK(mainB->current);
  CHECK(mainB->upstream.value_or("") == "origin/main");
  r.g({"checkout", "-q", "--detach", "HEAD~1"});
  st = git.status(r.repo);
  CHECK(st.detached);
  CHECK(st.branch.empty());
  for (const auto& b : git.branches(r.repo)) {
    CHECK(b.name.rfind('(', 0) != 0);  // no "(HEAD detached at ...)" pseudo-branch
    CHECK_FALSE(b.current);            // nothing is current on a detached HEAD
  }
}

TEST_CASE("real git: log keeps the subject and the body") {
  if (!haveGit()) return;
  ParseRepo r("log");
  RealGit rg{r.home, {}, {}};
  GitOps git([&](const std::string& c, const CommandOptions& o) { return rg.run(c, o); });
  r.write("f", "1");
  r.g({"add", "-A"});
  r.g({"commit", "-q", "-m", "subject line", "-m", "body paragraph 1\nsecond line", "-m", "paragraph 2"});
  r.write("f", "2");
  r.g({"commit", "-qam", "plain"});
  auto log = git.log(GitLogOptions{r.repo, std::nullopt});
  REQUIRE(log.size() == 2);
  CHECK(log[0].message == "plain");
  CHECK(log[0].body == "");
  CHECK(log[1].message == "subject line");
  CHECK(log[1].body == "body paragraph 1\nsecond line\n\nparagraph 2");
  CHECK(git.log(GitLogOptions{r.repo, 1}).size() == 1);
}

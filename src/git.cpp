#include "solari/git.hpp"

#include <regex>

#include "solari/errors.hpp"
#include "solari/util.hpp"

namespace solari {

static CommandOptions optsWith(const std::vector<std::string>& args,
                               const std::optional<std::string>& cwd,
                               const std::map<std::string, std::string>& env) {
  CommandOptions o;
  o.args = args;
  if (cwd) o.cwd = cwd;
  o.env = env;
  return o;
}

/** The git subcommand in an argv that may start with `-c k=v` pairs. */
static std::string gitSubcommand(const std::vector<std::string>& args) {
  std::size_t i = 0;
  while (i + 1 < args.size() && args[i] == "-c") i += 2;
  return i < args.size() ? args[i] : "";
}

/** User text that begins with "-" would be parsed by git as an option. */
static void rejectOptionLike(const char* what, const std::string& v) {
  if (!v.empty() && v[0] == '-')
    throw Error(std::string("git ") + what + " must not start with \"-\": " + v);
}

CommandResult GitOps::runGit(const std::vector<std::string>& args,
                             std::optional<std::string> cwd,
                             const std::map<std::string, std::string>& env) {
  return run_("git", optsWith(args, cwd, env));
}

std::string GitOps::mustGit(const std::vector<std::string>& args,
                            std::optional<std::string> cwd,
                            const std::map<std::string, std::string>& env,
                            const std::string& secret) {
  CommandResult r = runGit(args, cwd, env);
  if (r.exitCode != 0) {
    std::string detail = trim(!r.stderr_.empty() ? r.stderr_ : r.stdout_);
    if (!secret.empty()) {
      std::size_t pos = 0;
      while ((pos = detail.find(secret, pos)) != std::string::npos) {
        detail.replace(pos, secret.size(), "***");
        pos += 3;
      }
    }
    throw Error("git " + gitSubcommand(args) + " failed (exit " +
                std::to_string(r.exitCode) + "): " + detail);
  }
  return r.stdout_;
}

// Inline git credential helper: answers `get` from the command's environment, ignores `store` / `erase`, and holds
// no secret.
static const char* kCredentialHelper =
    "!f() { test \"$1\" = get && printf \"username=%s\\npassword=%s\\n\" "
    "\"$SOLARI_GIT_USERNAME\" \"$SOLARI_GIT_PASSWORD\"; }; f";

GitOps::Auth GitOps::authFor(const std::string& remoteUrl, const std::string& username,
                             const std::string& password) {
  Auth a;
  if (username.empty() && password.empty()) return a;
  auto hasBad = [](const std::string& v) {
    return v.find_first_of(std::string("\r\n\0", 3)) != std::string::npos;
  };
  if (hasBad(username) || hasBad(password))
    throw Error("git credentials must not contain line breaks or NUL");
  auto pos = remoteUrl.find("://");
  if (pos == std::string::npos) return a;  // scp-style or malformed: not an http(s) remote
  std::string scheme = remoteUrl.substr(0, pos);
  if (scheme != "http" && scheme != "https") return a;
  std::string rest = remoteUrl.substr(pos + 3);
  auto e = rest.find_first_of("/?#");
  std::string authority = (e == std::string::npos) ? rest : rest.substr(0, e);
  auto at = authority.rfind('@');
  std::string host = (at == std::string::npos) ? authority : authority.substr(at + 1);
  if (host.empty() || host.find_first_of("= \t\r\n") != std::string::npos) return a;
  std::string origin = scheme + "://" + host;
  a.ok = true;
  // The empty value first clears any helper configured on the guest, so only ours answers...
  a.cfg = {"-c", "credential.helper=",
           // ...and only for this origin.
           "-c", "credential." + origin + ".helper=" + kCredentialHelper};
  a.env = {{"GIT_TERMINAL_PROMPT", "0"},
           {"SOLARI_GIT_USERNAME", username},
           {"SOLARI_GIT_PASSWORD", password}};
  return a;
}

void GitOps::clone(const std::string& url, const GitCloneOptions& opts) {
  Auth auth = authFor(url, opts.username.value_or(""), opts.password.value_or(""));
  std::vector<std::string> args = auth.cfg;
  args.push_back("clone");
  if (opts.depth && *opts.depth > 0) {
    args.push_back("--depth");
    args.push_back(std::to_string(*opts.depth));
  }
  if (opts.branch) {
    args.push_back("--branch");
    args.push_back(*opts.branch);
  }
  // `--` ends options: a url such as "--upload-pack=cmd" is then a (bad) repository, not an option.
  args.push_back("--");
  args.push_back(url);
  if (opts.path) args.push_back(*opts.path);
  mustGit(args, opts.cwd, auth.env, opts.password.value_or(""));
}

GitStatus GitOps::status(std::optional<std::string> cwd) {
  std::string out = mustGit({"status", "--porcelain=v1", "--branch"}, cwd);
  GitStatus st;
  st.clean = true;
  std::size_t start = 0;
  while (start <= out.size()) {
    auto nl = out.find('\n', start);
    std::string line =
        out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (nl == std::string::npos) start = out.size() + 1;
    else start = nl + 1;
    if (line.empty()) continue;

    if (line.rfind("## ", 0) == 0) {
      std::string head = line.substr(3);
      if (head.rfind("HEAD (no branch)", 0) == 0) {
        st.detached = true;
      } else {
        std::string body = head;
        std::smatch m;
        std::regex noCommits("^No commits yet on (.+)$");
        if (std::regex_match(head, m, noCommits)) body = m[1].str();
        // branch = body before "..." then before " "
        std::string branch = body;
        auto dots = branch.find("...");
        if (dots != std::string::npos) branch = branch.substr(0, dots);
        auto sp = branch.find(' ');
        if (sp != std::string::npos) branch = branch.substr(0, sp);
        st.branch = branch;
        std::smatch ab;
        if (std::regex_search(head, ab, std::regex("\\[(.*)\\]"))) {
          std::string inner = ab[1].str();
          std::smatch a, b;
          if (std::regex_search(inner, a, std::regex("ahead (\\d+)")))
            st.ahead = std::stoi(a[1].str());
          if (std::regex_search(inner, b, std::regex("behind (\\d+)")))
            st.behind = std::stoi(b[1].str());
        }
      }
      continue;
    }

    std::string xy = line.substr(0, 2);
    std::string path = line.size() > 3 ? line.substr(3) : "";
    if (xy == "??") {
      st.untracked.push_back(path);
    } else if (xy.size() == 2) {
      char index = xy[0];
      char work = xy[1];
      if (index != ' ' && index != '?') st.staged.push_back(path);
      if (work != ' ' && work != '?') st.modified.push_back(path);
    }
  }
  st.clean = st.staged.empty() && st.modified.empty() && st.untracked.empty();
  return st;
}

void GitOps::add(const std::vector<std::string>& paths,
                 std::optional<std::string> cwd) {
  if (paths.empty()) return;  // no-op
  std::vector<std::string> args = {"add", "--"};
  args.insert(args.end(), paths.begin(), paths.end());
  mustGit(args, cwd);
}

GitCommitResult GitOps::commit(const std::string& message,
                               const GitCommitOptions& opts) {
  std::vector<std::string> args;
  if (opts.author) {
    args.push_back("-c");
    args.push_back("user.name=" + *opts.author);
  }
  if (opts.email) {
    args.push_back("-c");
    args.push_back("user.email=" + *opts.email);
  }
  args.push_back("commit");
  args.push_back("-m");
  args.push_back(message);
  if (opts.all) args.push_back("-a");
  mustGit(args, opts.cwd);
  std::string hash = trim(mustGit({"rev-parse", "HEAD"}, opts.cwd));
  return GitCommitResult{hash};
}

void GitOps::pushPull(const std::string& op, const GitRemoteOptions& opts) {
  std::string remote = opts.remote.value_or("");
  if (remote.empty()) remote = "origin";
  rejectOptionLike("remote", remote);
  if (opts.branch) rejectOptionLike("branch", *opts.branch);
  Auth auth;
  if (opts.username || opts.password) {
    // The helper is bound to the remote's origin, so resolve the remote's URL first.
    std::string plain = trim(mustGit({"remote", "get-url", "--", remote}, opts.cwd));
    auth = authFor(plain, opts.username.value_or(""), opts.password.value_or(""));
  }
  std::vector<std::string> args = auth.cfg;
  args.push_back(op);
  args.push_back("--");
  args.push_back(remote);
  if (opts.branch) args.push_back(*opts.branch);
  mustGit(args, opts.cwd, auth.env, opts.password.value_or(""));
}

void GitOps::push(const GitRemoteOptions& opts) { pushPull("push", opts); }
void GitOps::pull(const GitRemoteOptions& opts) { pushPull("pull", opts); }

void GitOps::checkout(const std::string& ref, const GitCheckoutOptions& opts) {
  // A ref that starts with "-" would be read as an option ("--orphan=x" created a branch). Git never allows one as a
  // branch name, so refuse it rather than guess.
  rejectOptionLike("ref", ref);
  std::vector<std::string> args = {"checkout"};
  if (opts.create) args.push_back("-b");
  args.push_back(ref);
  args.push_back("--");
  mustGit(args, opts.cwd);
}

std::vector<GitBranch> GitOps::branches(std::optional<std::string> cwd) {
  std::string out = mustGit(
      {"branch", "--format=%(HEAD)%1f%(refname:short)%1f%(objectname:short)"}, cwd);
  std::vector<GitBranch> branches;
  std::size_t start = 0;
  while (start <= out.size()) {
    auto nl = out.find('\n', start);
    std::string line =
        out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (nl == std::string::npos) start = out.size() + 1;
    else start = nl + 1;
    if (trim(line).empty()) continue;
    // split on \x1f into [head, name, commit]
    std::vector<std::string> f;
    std::size_t p = 0;
    while (true) {
      auto q = line.find('\x1f', p);
      if (q == std::string::npos) {
        f.push_back(line.substr(p));
        break;
      }
      f.push_back(line.substr(p, q - p));
      p = q + 1;
    }
    GitBranch b;
    b.current = f.size() > 0 && f[0] == "*";
    b.name = f.size() > 1 ? f[1] : "";
    b.commit = f.size() > 2 ? f[2] : "";
    branches.push_back(b);
  }
  return branches;
}

std::vector<GitCommit> GitOps::log(const GitLogOptions& opts) {
  std::vector<std::string> args = {"log", "--format=%H%x1f%an%x1f%ae%x1f%aI%x1f%s"};
  if (opts.maxCount && *opts.maxCount > 0)
    args.push_back("--max-count=" + std::to_string(*opts.maxCount));
  std::string out = mustGit(args, opts.cwd);
  std::vector<GitCommit> commits;
  std::size_t start = 0;
  while (start <= out.size()) {
    auto nl = out.find('\n', start);
    std::string line =
        out.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
    if (nl == std::string::npos) start = out.size() + 1;
    else start = nl + 1;
    if (trim(line).empty()) continue;
    std::vector<std::string> f;
    std::size_t p = 0;
    while (true) {
      auto q = line.find('\x1f', p);
      if (q == std::string::npos) {
        f.push_back(line.substr(p));
        break;
      }
      f.push_back(line.substr(p, q - p));
      p = q + 1;
    }
    GitCommit c;
    c.hash = f.size() > 0 ? f[0] : "";
    c.author = f.size() > 1 ? f[1] : "";
    c.email = f.size() > 2 ? f[2] : "";
    c.date = f.size() > 3 ? f[3] : "";
    c.message = f.size() > 4 ? f[4] : "";
    commits.push_back(c);
  }
  return commits;
}

} // namespace solari

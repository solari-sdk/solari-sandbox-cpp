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

static std::vector<std::string> splitOn(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::size_t p = 0;
  while (true) {
    auto q = s.find(sep, p);
    if (q == std::string::npos) {
      out.push_back(s.substr(p));
      return out;
    }
    out.push_back(s.substr(p, q - p));
    p = q + 1;
  }
}

static int numberAfter(const std::string& s, const std::string& marker) {
  auto at = s.find(marker);
  if (at == std::string::npos) return 0;
  return std::atoi(s.c_str() + at + marker.size());
}

/** `main...origin/main [ahead 1, behind 2]` | `main...origin/main [gone]` | `main` | `No commits yet on main` | `HEAD (no branch)`. */
static void parseStatusHeader(std::string head, GitStatus& st) {
  if (head.rfind("HEAD (no branch)", 0) == 0) {
    st.detached = true;
    return;
  }
  for (const char* prefix : {"No commits yet on ", "Initial commit on "})
    if (head.rfind(prefix, 0) == 0) head = head.substr(std::string(prefix).size());
  std::string refs = head, counts;
  auto br = head.find(" [");
  if (br != std::string::npos) {
    refs = head.substr(0, br);
    counts = head.substr(br + 2);
  }
  auto dots = refs.find("...");
  if (dots != std::string::npos) {
    st.branch = refs.substr(0, dots);
    st.upstream = refs.substr(dots + 3);
  } else {
    st.branch = refs;
  }
  st.ahead = numberAfter(counts, "ahead ");
  st.behind = numberAfter(counts, "behind ");
}

/** Parse `git status --porcelain=v1 -z --branch --untracked-files=all`: NUL-terminated records, the first `## branch...upstream
 *  [ahead N, behind M]`, then `XY path`, where a rename or copy (X or Y is R/C) is followed by one more record holding the ORIGINAL
 *  path. Paths are verbatim (-z turns quoting off). */
static GitStatus parseStatus(const std::string& out) {
  GitStatus st;
  std::vector<std::string> recs;
  for (auto& r : splitOn(out, '\0'))
    if (!r.empty()) recs.push_back(std::move(r));
  for (std::size_t i = 0; i < recs.size(); i++) {
    const std::string& rec = recs[i];
    if (rec.rfind("## ", 0) == 0) {
      parseStatusHeader(rec.substr(3), st);
      continue;
    }
    if (rec.size() < 3) continue;
    const char x = rec[0], y = rec[1];
    GitStatusEntry e;
    e.path = rec.substr(3);
    e.index = x;
    e.worktree = y;
    if (x == 'R' || x == 'C' || y == 'R' || y == 'C') {
      if (i + 1 < recs.size()) e.origPath = recs[++i];
    }
    if (x == '!' && y == '!') continue;  // ignored files are not part of the status
    const std::string xy{x, y};
    if (xy == "??") {
      st.untracked.push_back(e.path);
    } else if (xy == "DD" || xy == "AU" || xy == "UD" || xy == "UA" || xy == "DU" || xy == "AA" || xy == "UU") {
      st.conflicted.push_back(e.path);
    } else {
      if (x != ' ') st.staged.push_back(e.path);
      if (y != ' ') st.modified.push_back(e.path);
    }
    st.entries.push_back(std::move(e));
  }
  st.clean = st.entries.empty();
  return st;
}

GitStatus GitOps::status(std::optional<std::string> cwd) {
  return parseStatus(mustGit({"status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all"}, cwd));
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
  // for-each-ref lists real branches only: a detached HEAD has no refs/heads entry, so it is not a branch and nothing is current.
  std::string out = mustGit(
      {"for-each-ref", "--format=%(HEAD)%1f%(refname:short)%1f%(objectname:short)%1f%(upstream:short)", "refs/heads"}, cwd);
  std::vector<GitBranch> branches;
  for (const std::string& line : splitOn(out, '\n')) {
    if (line.empty()) continue;
    std::vector<std::string> f = splitOn(line, '\x1f');
    GitBranch b;
    b.current = !f.empty() && f[0] == "*";
    b.name = f.size() > 1 ? f[1] : "";
    b.commit = f.size() > 2 ? f[2] : "";
    if (f.size() > 3 && !f[3].empty()) b.upstream = f[3];
    branches.push_back(std::move(b));
  }
  return branches;
}

std::vector<GitCommit> GitOps::log(const GitLogOptions& opts) {
  std::vector<std::string> args = {"log", "--format=%H%x1f%an%x1f%ae%x1f%aI%x1f%s%x1f%b%x1e"};
  if (opts.maxCount && *opts.maxCount > 0)
    args.push_back("--max-count=" + std::to_string(*opts.maxCount));
  std::string out;
  try {
    out = mustGit(args, opts.cwd);
  } catch (const Error& e) {
    // A repository with no commit yet is an empty history, not an error.
    if (std::string(e.what()).find("does not have any commits yet") != std::string::npos) return {};
    throw;
  }
  // Records end with \x1e, fields with \x1f, so a multi-line body belongs to its own commit.
  std::vector<GitCommit> commits;
  for (std::string rec : splitOn(out, '\x1e')) {
    while (!rec.empty() && rec.front() == '\n') rec.erase(rec.begin());
    if (rec.empty()) continue;
    std::vector<std::string> f;
    std::size_t p = 0;
    for (int i = 0; i < 5; i++) {  // five fields end at \x1f; the rest (the body) may itself hold anything but \x1e
      auto q = rec.find('\x1f', p);
      if (q == std::string::npos) break;
      f.push_back(rec.substr(p, q - p));
      p = q + 1;
    }
    f.push_back(p <= rec.size() ? rec.substr(p) : "");
    GitCommit c;
    c.hash = f.size() > 0 ? f[0] : "";
    c.author = f.size() > 1 ? f[1] : "";
    c.email = f.size() > 2 ? f[2] : "";
    c.date = f.size() > 3 ? f[3] : "";
    c.message = f.size() > 4 ? f[4] : "";
    c.body = f.size() > 5 ? f[5] : "";
    while (!c.body.empty() && c.body.back() == '\n') c.body.pop_back();
    commits.push_back(std::move(c));
  }
  return commits;
}

} // namespace solari

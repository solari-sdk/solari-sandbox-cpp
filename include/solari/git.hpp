// git namespace: safe, non-shell `git` invocations over the command RPC, plus
// the client-side parsers (status/branches/log) and arg construction
// (clone/commit/push auth). Ported 1:1 from `@solarisdk/core` handle.ts `git`.
//
// GitOps runs against an injected CommandRunner (`git <args>` -> CommandResult),
// which is `Sandbox::runCommand` in production and a canned stub in tests —
// exactly how the reference git.test.mjs stubs `runCommand`.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "solari/types.hpp"

namespace solari {

/** Runs `git` with `opts.args` (never a shell); returns its CommandResult. */
using CommandRunner =
    std::function<CommandResult(const std::string& cmd, const CommandOptions&)>;

class GitOps {
 public:
  explicit GitOps(CommandRunner run) : run_(std::move(run)) {}

  void clone(const std::string& url, const GitCloneOptions& opts = {});
  GitStatus status(std::optional<std::string> cwd = std::nullopt);
  void add(const std::vector<std::string>& paths,
           std::optional<std::string> cwd = std::nullopt);
  GitCommitResult commit(const std::string& message,
                         const GitCommitOptions& opts = {});
  void push(const GitRemoteOptions& opts = {});
  void pull(const GitRemoteOptions& opts = {});
  void checkout(const std::string& ref, const GitCheckoutOptions& opts = {});
  std::vector<GitBranch> branches(std::optional<std::string> cwd = std::nullopt);
  std::vector<GitCommit> log(const GitLogOptions& opts = {});

  /**
   * Credentials for ONE git command against `remoteUrl`: `-c` options and environment. The credentials travel as
   * environment variables of that command and git reads them back through an inline credential helper, so they are
   * never in the URL (and so not in .git/config, a snapshot or a template), never in argv (/proc/<pid>/cmdline) and
   * never in a config file. The helper is bound to the remote's origin (`credential.<scheme>://<host>.helper`), so git
   * never offers the password to another host. `ok` is false when there are no credentials or the remote is not
   * http(s). Public for testing.
   */
  struct Auth {
    bool ok = false;
    std::vector<std::string> cfg;
    std::map<std::string, std::string> env;
  };
  static Auth authFor(const std::string& remoteUrl, const std::string& username,
                      const std::string& password);

 private:
  CommandRunner run_;
  CommandResult runGit(const std::vector<std::string>& args,
                       std::optional<std::string> cwd,
                       const std::map<std::string, std::string>& env = {});
  std::string mustGit(const std::vector<std::string>& args,
                      std::optional<std::string> cwd,
                      const std::map<std::string, std::string>& env = {},
                      const std::string& secret = "");
  void pushPull(const std::string& op, const GitRemoteOptions& opts);
};

} // namespace solari

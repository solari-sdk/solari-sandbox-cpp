// git namespace: safe, non-shell `git` invocations over the command RPC, plus
// the client-side parsers (status/branches/log) and arg construction
// (clone/commit/push auth). Ported 1:1 from `@solarisdk/core` handle.ts `git`.
//
// GitOps runs against an injected CommandRunner (`git <args>` -> CommandResult),
// which is `Sandbox::runCommand` in production and a canned stub in tests —
// exactly how the reference git.test.mjs stubs `runCommand`.
#pragma once
#include <functional>
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

  /** Splice basic-auth creds into an https remote URL (public for testing). */
  static std::string authUrl(const std::string& url, const std::string& username,
                             const std::string& password);

 private:
  CommandRunner run_;
  CommandResult runGit(const std::vector<std::string>& args,
                       std::optional<std::string> cwd);
  std::string mustGit(const std::vector<std::string>& args,
                      std::optional<std::string> cwd);
  void pushPull(const std::string& op, const GitRemoteOptions& opts);
};

} // namespace solari

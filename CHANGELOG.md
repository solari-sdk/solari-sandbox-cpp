# Changelog

## 0.3.1

### Fixed
- **A deleted sandbox no longer reports "not connected".** A `404` on the one-shot `/exec` route is only read as "route not served" (and retried over the control channel) when it is a plain-text 404; a JSON `404` is the gateway saying the sandbox is gone, and now reaches you as that `GatewayError`.
- **`CommandResult::timedOut`** says a command hit its `timeoutMs` and was killed (`exitCode` -1). The one-shot route reported it and it was dropped; the control-channel path now sets it too.
- **`Sandbox::recordingUrl()`** (and `CreateSandboxResponse::recordingUrl`) carry the recording link from a create that asked for a recording (it was discarded).
- **Security: `GitOps::clone` / `push` / `pull` credentials no longer reach the clone URL, `.git/config` or the process list.** With a username/password, `clone` used to put them in the URL, so git saved them as `remote.origin.url` (and from there into snapshots and templates) and showed them in `/proc/*/cmdline`; push and pull put them in a `-c url.….insteadOf=` argument. They now travel as environment variables of that one command and git reads them through an inline credential helper bound to the remote's origin, so nothing is left in the repository, in a config file or on a command line, and git never offers the password to another host (a redirect to a different host gets nothing). If you cloned a private repo with credentials before this release, check `.git/config` in existing snapshots and templates and rotate the token if it is there. A username or password with a line break is refused, and the password is masked in error messages. Authenticated git needs the live control connection, so on a `shared` sandbox only public remotes work.
- **git commands no longer read user text as options.** `clone`, `push`, `pull` and `checkout` put `--` before the repository, remote and ref, and refuse a remote, branch or ref that starts with `-` (`clone("--upload-pack=cmd")` ran `cmd` in the sandbox; `checkout("--orphan=x")` created a branch).

### Changed
- `GitOps::authUrl` is replaced by `GitOps::authFor`: the old function built the URL with the password in it, which is what this release stops doing. Nothing else in the public API changes.

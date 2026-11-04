# Changelog

## 0.3.3

### Fixed
- **`commands.run` on a shared-isolation sandbox works when the control channel is connected.** A shared sandbox refuses command streams (`cmd.start`, error `NotAvailableOnSharedIsolation`) but runs commands over REST; `run` on a connected channel sent `cmd.start` and threw the refusal. It now runs that command over REST `/exec` and remembers that the sandbox is shared, so later runs go straight to REST (the same behaviour as the TypeScript and Python SDKs). A run that needs a stream (an output callback, `env`, `user`, `background`, a `cancel` token) still throws the refusal.

## 0.3.2

### Added
- **`files.upload` and `files.download`** move a file in 4 MiB chunks (`fs.upload` / `fs.download`), so a large file never travels as one frame. A 48 MiB file used to be a 64 MiB frame that closed the control channel and failed every call in flight on it. `files.read` now reads through the same chunked loop (a guest image that predates `fs.download` is read with `fs.read`).
- **`CancelToken`** (`CommandOptions::cancel`, `RunCodeOptions::cancel`): call `cancel()` from another thread and the run stops the guest process (SIGTERM and a closed stdin at once, SIGKILL 2 s later; `code.interrupt` for code, ignored by a guest that does not know it) and throws `CancelledError`. A cancellable `commands.run` always uses the control channel. `CommandHandle::closeStdin()` closes a command's stdin.

### Changed
- `files.write` refuses more than 32 MiB before sending anything (`file too large for a single call (N bytes, max 33554432): use files.upload()/files.download() or uploadUrl()`), so one big call fails alone and the channel stays up. Use `upload` for larger files.

### Fixed
- **A callback may call back into the SDK.** Output callbacks ran on the rx thread with the command's mutex held, so a callback that called `onData` deadlocked, and one that called `stdin_write` or read a file stalled the thread that had to deliver the reply. Each command now has a delivery thread of its own that runs the callbacks and settles the exit in order, with no lock held; `wait()` returns after the last callback.
- **Concurrent `Sandbox::connect()` calls open one socket,** not one each (the extras leaked with their reader threads).

## 0.3.1

### Fixed
- **A deleted sandbox no longer reports "not connected".** A `404` on the one-shot `/exec` route is only read as "route not served" (and retried over the control channel) when it is a plain-text 404; a JSON `404` is the gateway saying the sandbox is gone, and now reaches you as that `GatewayError`.
- **`CommandResult::timedOut`** says a command hit its `timeoutMs` and was killed (`exitCode` -1). The one-shot route reported it and it was dropped; the control-channel path now sets it too.
- **`Sandbox::recordingUrl()`** (and `CreateSandboxResponse::recordingUrl`) carry the recording link from a create that asked for a recording (it was discarded).
- **Security: `GitOps::clone` / `push` / `pull` credentials no longer reach the clone URL, `.git/config` or the process list.** With a username/password, `clone` used to put them in the URL, so git saved them as `remote.origin.url` (and from there into snapshots and templates) and showed them in `/proc/*/cmdline`; push and pull put them in a `-c url.….insteadOf=` argument. They now travel as environment variables of that one command and git reads them through an inline credential helper bound to the remote's origin, so nothing is left in the repository, in a config file or on a command line, and git never offers the password to another host (a redirect to a different host gets nothing). If you cloned a private repo with credentials before this release, check `.git/config` in existing snapshots and templates and rotate the token if it is there. A username or password with a line break is refused, and the password is masked in error messages. Authenticated git needs the live control connection, so on a `shared` sandbox only public remotes work.
- **git commands no longer read user text as options.** `clone`, `push`, `pull` and `checkout` put `--` before the repository, remote and ref, and refuse a remote, branch or ref that starts with `-` (`clone("--upload-pack=cmd")` ran `cmd` in the sandbox; `checkout("--orphan=x")` created a branch).

### Changed
- `GitOps::authUrl` is replaced by `GitOps::authFor`: the old function built the URL with the password in it, which is what this release stops doing. Nothing else in the public API changes.

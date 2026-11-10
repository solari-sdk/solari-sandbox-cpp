# Changelog

## 0.5.1

### Fixed
- **An undecodable `stdoutBase64` / `stderrBase64` on a `/exec` answer no longer empties `stdoutBytes` / `stderrBytes`.** The lenient base64 decoder skipped stray characters and returned garbage or nothing; the bytes now fall back to the text's bytes, as the Go and Rust SDKs do. The gateway only sends these fields for output that is not valid UTF-8, so a well-formed answer was never affected.

### Documented
- **`files.write` creates a missing parent directory** (checked live on staging: writing `/tmp/no-such-dir/deeper/a.txt` succeeds and reads back). It does not throw; the SDK reference said it did. The header comment now says so.

### Tests
- Added for the retest: exact stdout and stderr bytes over the live connection (kept apart from each other); the `/exec` answer's bytes (base64 per stream, the text as the fallback, a bad base64 falls back per stream); every refusal before git runs (option-like ref, remote and branch, a credential with a line break) throws and sends nothing. `detail::parseCommandResult` is exposed in `client.hpp` so the `/exec` parsing can be tested without a server.

### Upgrading from 0.4 (behaviour changes since 0.5.0 that can break callers)
- **`stdout_` / `stderr_` are UTF-8 text** decoded over the whole stream (invalid bytes become U+FFFD). They used to be the raw bytes on the live connection; read `stdoutBytes` / `stderrBytes` for the exact bytes.
- **A deadline that fires below the guest is a result, not an error** (`exitCode -1`, `timedOut`); code that caught the `400 ExecTimeout` or `502 ... i/o timeout` gateway error no longer sees it.
- **A command that cannot start is `ActionError` with `method "cmd.start"` and `code "ExecFailed"`** on both routes; `/exec` used to throw a `GatewayError` for it.
- **A command that needs the live connection opens it by itself** (`env`, `user`, callbacks, a cancel token or `background`); it used to throw "Not connected".
- **`user` is validated before any transport is chosen** (`ValidationError`), and `args` is always sent (an empty list when there are none).
- `Git.status`, `branches` and `log` parse differently since 0.5/0.6 (verbatim paths, only real branches, an empty list for a repository with no commit).

## 0.5.0

`commands.run` now behaves the same over the one-shot `/exec` route and the live control connection (the shared conformance rows in `tests/conformance/spec/commands.json` pin it; before this release 13 of them failed for this SDK).

### Changed
- **A command that needs the live connection opens it by itself.** `env`, `user`, callbacks, a cancel token or `background` used to throw "Not connected — call connect() first" on a sandbox whose channel was not open yet, and so did the fall back to the control connection when the gateway does not serve `/exec`. They now connect on demand, as the REST route is chosen without the caller doing anything.
- **A deadline that fires below the guest is a result, not an error.** `400 ExecTimeout` and the guest's hidden vsock read timeout (`502 ... i/o timeout`) on `/exec` now return `CommandResult{exitCode -1, timedOut true}`, the same result the live connection gives when `timeoutMs` elapses.
- **A command that cannot start is one error on both routes:** `ActionError` with `method "cmd.start"` and `code "ExecFailed"`. `/exec` reports it as a `400 ExecFailed` gateway error, which used to come back as `GatewayError`; the live connection's refusal carries no code, and `fork/exec ...` / `chdir ...` now get `ExecFailed`.
- **`stdout_` / `stderr_` are UTF-8 text** decoded over the whole stream (a character split across two chunks survives; invalid bytes become U+FFFD; NUL is kept). They used to be the raw bytes on the live connection, which is not valid JSON or text for a binary command. Read exact bytes from the new `stdoutBytes` / `stderrBytes`. Callbacks still receive the raw chunks.
- `args` is always in the `/exec` body and in `cmd.start` (an empty list when there are none).

### Fixed
- **`git.status()`, `branches()` and `log()` no longer lose information** (they ran against real git for the first time in this release's tests). `status` uses `--porcelain=v1 -z --branch --untracked-files=all`: paths are verbatim (not quoted or escaped), a rename is its NEW path with the old one in `entries[].origPath`, an untracked directory lists its files, a merge conflict is listed once in `conflicted` (not as staged and modified), and `upstream`, `ahead`/`behind` (also `[gone]`) and a detached HEAD are read correctly. New fields: `GitStatus::upstream`, `conflicted`, `entries` (`GitStatusEntry`). `branches` uses `for-each-ref refs/heads`: a detached HEAD is no longer a branch named `(HEAD detached at ...)` and nothing is current then; `GitBranch::upstream` is new. `log` keeps the message body (`GitCommit::body`, `message` is the subject), splits records on `\x1e` so a multi-line body stays with its commit, and a repository with no commit returns an empty list instead of throwing.

### Added
- **`ValidationError`** (a `GatewayError`: status 400, code `"ValidationError"`) is thrown before any request when `user` is neither `root` nor a unix user name (`^[a-z_][a-z0-9_-]{0,31}$`) or `timeoutMs` is negative, the same error the gateway's `/exec` route gives, so a bad value fails the same way on both routes.
- **A `409 HostCapabilityMissing` from `/exec` is retried once on the live connection.** It means the host pool lacks `exec.user` for the sandbox's default user and nothing ran; the live connection reaches the guest directly, which honours `user`.
- **`CommandResult::oomKilled`**: true when the guest's memory watchdog killed the command (exit 137 alone is ambiguous). Always present.
- **`CommandResult::stdoutBytes` / `stderrBytes`**: the exact output bytes. Exact on the live connection. On `/exec` they are exact when the gateway sends its `stdoutBase64` / `stderrBase64` fields (it does, once its guests have them, for output that is not valid UTF-8); until then they are the bytes of the lossy text.
- `solari::utf8Lossy()` in `solari/util.hpp`.

## 0.4.0

### Added
- **`SandboxNotStoppedError`** (a `GatewayError`, status 409) is thrown for a 409 `NotStopped` or `StartInProgress` from `Client::start()` / `Sandbox::start()`. `code` is `"NotStopped"` (the sandbox is in another state; use `resume()` for a paused one) or `"StartInProgress"` (a start is already under way: poll until the sandbox is running), and `state` holds the sandbox's current state when the gateway sends it. Other 409s (`StoppedDiskMissing`, `GoldenMismatch`, `HostCapabilityMissing`, `SnapshotNotRestorable`, ...) stay a plain `GatewayError`. This is the same type the TypeScript and Python SDKs expose. Code that caught `GatewayError` keeps working; the new class is a subclass.

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

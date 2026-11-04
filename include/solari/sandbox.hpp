// `Sandbox` — a live session handle. Wraps a ControlChannel and exposes the core
// surface: `.commands` (run/start/stdin/kill), `.files` (read/write/list/stat/
// mkdir/remove/rename), `.code` (run over code.run with chart flatten), and
// `.git`. Blocking synchronous API; async output arrives on the channel's rx
// thread as v2 cmd.data/cmd.exit frames. Ported from `@solarisdk/core` handle.ts.
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "solari/code.hpp"
#include "solari/control_channel.hpp"
#include "solari/git.hpp"
#include "solari/types.hpp"

namespace solari {

/** Gateway-routed calls a handle delegates back to its owning client. */
struct SessionHooks {
  std::function<CommandResult(const std::string& id, const ExecOneShotRequest&)> exec;
  std::function<void(const std::string& id)> kill;
  /** `POST /sandboxes/:id/pause`. */
  std::function<void(const std::string& id)> pause;
  /** `POST /sandboxes/:id/resume` -> the control URL to re-attach to. */
  std::function<std::string(const std::string& id)> resume;
  /** `POST /sandboxes/:id/start` -> the fresh control URL of the cold-booted sandbox. */
  std::function<std::string(const std::string& id)> start;
};

struct SandboxConfig {
  std::string apiKey;
  long callTimeoutMs = 300000;
  SessionHooks hooks;
};

/**
 * Shared state of a started command; mutated by the rx thread + the caller.
 *
 * The rx thread only queues events (`push`); a delivery thread of the command's own runs the callbacks and settles the
 * exit, in order, with no lock held. A callback may therefore call back into the SDK (onData, stdin_write, a file read)
 * without deadlocking or stalling the rx thread that has to deliver the reply.
 */
struct CmdState {
  std::mutex m;  // guards cbs, buffered, settled
  std::vector<std::function<void(const std::string& stream, const std::string& data)>>
      cbs;
  std::vector<std::pair<std::string, std::string>> buffered;  // stream,data
  std::promise<int> exitProm;
  std::shared_future<int> exitFut;
  bool settled = false;

  struct Event {
    bool isExit = false;
    std::string stream, data;
    int code = 0;
    std::exception_ptr err;
  };
  std::mutex qm;
  std::condition_variable qcv;
  std::deque<Event> q;
  bool exitQueued = false;

  /** Queue an event for the delivery thread; events after the exit are dropped. */
  void push(Event ev) {
    std::lock_guard<std::mutex> lk(qm);
    if (exitQueued) return;
    if (ev.isExit) exitQueued = true;
    q.push_back(std::move(ev));
    qcv.notify_one();
  }
};

/** A started command (`commands.start`). */
class CommandHandle {
 public:
  CommandHandle(ControlChannel* ch, std::string cmdId, std::shared_ptr<CmdState> st,
                long callTimeoutMs)
      : ch_(ch), cmdId_(std::move(cmdId)), st_(std::move(st)),
        callTimeoutMs_(callTimeoutMs) {}

  const std::string& cmdId() const { return cmdId_; }

  /** Subscribe to stdout/stderr chunks (replays anything buffered so far). */
  void onData(std::function<void(const std::string& stream, const std::string& data)> cb);
  /** Block until the command exits; returns its exit code. Throws on channel drop. */
  int wait();
  /** Like wait(), but gives up after `timeoutMs`: returns the exit code, or nullopt on timeout. */
  std::optional<int> waitFor(long timeoutMs);
  /** Write bytes to the command's stdin. */
  void stdin_write(const std::string& data);
  /** Send a signal (0 => server default). */
  void kill(int signal = 0);
  /** Close the command's stdin (the guest writes nothing, then closes it). */
  void closeStdin();

 private:
  ControlChannel* ch_;
  std::string cmdId_;
  std::shared_ptr<CmdState> st_;
  long callTimeoutMs_;
};

class Sandbox {
 public:
  Sandbox(const CreateSandboxResponse& resp, SandboxConfig config);
  ~Sandbox();
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  const std::string& id() const { return id_; }
  const std::string& controlUrl() const { return controlUrl_; }
  const std::string& expiresAt() const { return expiresAt_; }
  /** The VM flavour — "sandbox" or "desktop". */
  const std::string& kind() const { return kind_; }
  /** The live-view stream URL. Set for desktops only — a headless sandbox has
   *  no display, so the gateway omits it. */
  const std::optional<std::string>& streamUrl() const { return streamUrl_; }
  /** The session recording link, when the create asked for a recording. */
  const std::optional<std::string>& recordingUrl() const { return recordingUrl_; }

  /** Open the control WebSocket (IXWebSocket). No-op / throws when built
   *  without SOLARI_WITH_WS. Idempotent. */
  void connect();
  /** Close the control channel locally (does not release the remote session). */
  void close();
  bool connected() const { return channel_->connected(); }
  /** Destroy the remote session (via the kill hook) and close the channel. */
  void kill();

  /**
   * Pause this session: snapshot its RAM+disk, free its host slot, and close the
   * control channel. The session keeps its id; bring it back with resume().
   *
   * Mirrors TS `handle.pause()` / Python `handle.pause()`: remote call first,
   * then the local channel close. Throws when the handle has no pause hook (a
   * hand-built handle rather than a client-created one).
   */
  void pause();
  /**
   * Resume this paused session, adopt the control URL of the fresh slot it came
   * back on, and reopen the control channel.
   *
   * Mirrors TS `handle.resume()` / Python `handle.resume()`, which both end in an
   * unconditional `channel.reconnect()` — so this reconnects too rather than
   * leaving the caller holding a resumed VM and a dead channel.
   */
  void resume();
  /**
   * Cold-boot this STOPPED session from the disk the host kept, adopt the fresh
   * control URL, and reopen the control channel. Processes and memory are gone;
   * files on disk are kept. Throws GatewayError (status 409, `code` set) when the
   * sandbox is not stopped or its disk cannot be started; see Client::start().
   * Throws when the handle has no start hook (a hand-built handle).
   */
  void start();

  /** Direct channel access (for the WS adapter and unit tests). */
  ControlChannel& channel() { return *channel_; }

  // --- primitives -----------------------------------------------------------
  CommandResult runCommand(const std::string& cmd, const CommandOptions& opts = {});
  CommandHandle startCommand(const std::string& cmd, const CommandOptions& opts = {});

 private:
  /** The gateway refused a command stream: this sandbox is on the shared tier (commands then run over REST). */
  std::atomic<bool> shared_{false};
  // Internal: `tap` is subscribed before any frame handler is registered, so output that beat
  // the cmd.start reply reaches it as well as the caller's callbacks (see runCommand).
  CommandResult waitCancellable(CommandHandle& h, const CommandOptions& opts,
                                const std::shared_ptr<std::string>& out,
                                const std::shared_ptr<std::string>& err);
  CommandHandle startCommandTapped(
      const std::string& cmd, const CommandOptions& opts,
      std::function<void(const std::string&, const std::string&)> tap);

 public:
  RunCodeResult runCode(const std::string& code, const RunCodeOptions& opts = {});

  // --- namespaced accessors -------------------------------------------------
  class Commands {
   public:
    explicit Commands(Sandbox& s) : s_(s) {}
    CommandResult run(const std::string& cmd, const CommandOptions& o = {}) {
      return s_.runCommand(cmd, o);
    }
    CommandHandle start(const std::string& cmd, const CommandOptions& o = {}) {
      return s_.startCommand(cmd, o);
    }
   private:
    Sandbox& s_;
  };

  class Files {
   public:
    explicit Files(Sandbox& s) : s_(s) {}
    std::string read(const std::string& path);      // raw bytes
    std::string readText(const std::string& path);  // == read (UTF-8)
    /** Write in one frame. More than 32 MiB is refused before anything is sent: use upload(). */
    void write(const std::string& path, const std::string& data,
               std::optional<int> mode = std::nullopt);
    /** Write in 4 MiB chunks (fs.upload); any size. An empty file is one zero-length call. */
    void upload(const std::string& path, const std::string& data);
    /** Read a whole file in 4 MiB chunks (fs.download) until the guest reports the end. */
    std::string download(const std::string& path);
    std::vector<FsEntry> list(const std::string& path);
    FsStat stat(const std::string& path);
    void mkdir(const std::string& path);
    void remove(const std::string& path, bool recursive = false);
    void rename(const std::string& from, const std::string& to);
   private:
    Sandbox& s_;
  };

  class Code {
   public:
    explicit Code(Sandbox& s) : s_(s) {}
    RunCodeResult run(const std::string& code, const RunCodeOptions& o = {}) {
      return s_.runCode(code, o);
    }
   private:
    Sandbox& s_;
  };

  Commands commands{*this};
  Files files{*this};
  Code code{*this};
  GitOps git;

 private:
  long timeout() const { return callTimeoutMs_; }

  std::string id_;
  std::string controlUrl_;
  std::string expiresAt_;
  std::string kind_;
  std::optional<std::string> streamUrl_;
  std::optional<std::string> recordingUrl_;
  std::string apiKey_;
  long callTimeoutMs_;
  SessionHooks hooks_;
  std::shared_ptr<ControlChannel> channel_;
  std::mutex connectMu_;      // serialises connect(): concurrent callers each opened a socket and leaked the others
  std::shared_ptr<void> ws_;  // WsControlTransport when SOLARI_WITH_WS
};

} // namespace solari

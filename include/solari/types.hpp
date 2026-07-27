// Wire types for the Solari core surface, mirroring PROTOCOL.md and the
// reference `@solarisdk/core` types.ts. `Chart.elements` is kept as raw
// nlohmann::json so new chart types never force an SDK bump.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace solari {

using Json = nlohmann::json;

// --- commands ---------------------------------------------------------------

/** Terminal result of a command run. */
struct CommandResult {
  int exitCode = 0;
  std::string stdout_;
  std::string stderr_;
};

/** Options for `commands.run` / `commands.start`. */
struct CommandOptions {
  std::vector<std::string> args;
  std::optional<std::string> cwd;
  std::map<std::string, std::string> env;
  std::optional<std::string> user;
  std::optional<long> timeoutMs;
  bool background = false;
  std::function<void(const std::string&)> onStdout;
  std::function<void(const std::string&)> onStderr;
};

/** Body for the one-shot `POST /sandboxes/:id/exec` fast path. */
struct ExecOneShotRequest {
  std::string cmd;
  std::vector<std::string> args;
  std::optional<std::string> cwd;
  std::optional<long> timeoutMs;
};

// --- files ------------------------------------------------------------------

/** One `fs.list` entry. Wire shape (from types.ts): {name, dir, size}. */
struct FsEntry {
  std::string name;
  bool dir = false;
  std::int64_t size = 0;
};

/** `fs.stat` result. Wire shape: {name, dir, size, mode, modTimeMs}. */
struct FsStat {
  std::string name;
  bool dir = false;
  std::int64_t size = 0;
  int mode = 0;
  std::int64_t modTimeMs = 0;
};

// --- code.run ---------------------------------------------------------------

/** One axis of a 2D chart. */
struct ChartAxis {
  std::optional<std::string> label;
  std::optional<std::string> scale;
  std::vector<Json> ticks;  // numbers or strings, kept loose
  bool present = false;
};

/** Structured matplotlib figure. `elements` stays raw. */
struct Chart {
  std::string type;  // line|scatter|bar|pie|box_and_whisker|composite|unknown
  std::optional<std::string> title;
  std::optional<std::string> xLabel;
  std::optional<std::string> yLabel;
  ChartAxis x;
  ChartAxis y;
  Json elements = Json::array();
};

/** One rich result item from `code.run`. */
struct CodeResultItem {
  std::string type;  // stdout|stderr|result
  std::optional<std::string> text;
  std::optional<std::string> png;
  std::optional<std::string> jpeg;
  std::optional<std::string> svg;
  std::optional<std::string> html;
  std::optional<std::string> latex;
  std::optional<std::string> markdown;
  Json json_;                 // arbitrary
  std::optional<Chart> chart;
};

/** Result of `code.run`, with charts flattened out of `results`. */
struct RunCodeResult {
  std::vector<CodeResultItem> results;
  std::vector<Chart> charts;
  std::optional<Json> error;  // {name?,message?,traceback?} or string
};

/** Options for `runCode`. */
struct RunCodeOptions {
  std::optional<std::string> language;   // python|javascript|typescript|bash|r
  std::optional<std::string> contextId;
  std::function<void(const std::string&)> onStdout;
  std::function<void(const std::string&)> onStderr;
};

// --- git --------------------------------------------------------------------

struct GitStatus {
  std::string branch;
  bool detached = false;
  int ahead = 0;
  int behind = 0;
  std::vector<std::string> staged;
  std::vector<std::string> modified;
  std::vector<std::string> untracked;
  bool clean = true;
};

struct GitBranch {
  std::string name;
  std::string commit;
  bool current = false;
};

struct GitCommit {
  std::string hash;
  std::string author;
  std::string email;
  std::string date;
  std::string message;
};

struct GitCommitResult {
  std::string hash;
};

struct GitCloneOptions {
  std::optional<std::string> path;
  std::optional<std::string> branch;
  std::optional<int> depth;
  std::optional<std::string> username;
  std::optional<std::string> password;
  std::optional<std::string> cwd;
};

struct GitCommitOptions {
  std::optional<std::string> cwd;
  std::optional<std::string> author;
  std::optional<std::string> email;
  bool all = false;
};

struct GitRemoteOptions {
  std::optional<std::string> cwd;
  std::optional<std::string> remote;
  std::optional<std::string> branch;
  std::optional<std::string> username;
  std::optional<std::string> password;
};

struct GitCheckoutOptions {
  std::optional<std::string> cwd;
  bool create = false;
};

struct GitLogOptions {
  std::optional<std::string> cwd;
  std::optional<int> maxCount;
};

// --- REST lifecycle ---------------------------------------------------------

/** `kind` of a session. */
enum class SandboxKind { Sandbox, Desktop };
inline const char* toString(SandboxKind k) {
  return k == SandboxKind::Desktop ? "desktop" : "sandbox";
}

/** Idle lifecycle policy: what the gateway does when the idle timeout fires.
 *  `onTimeout:"pause"` snapshots RAM+disk and frees the slot (recover it with
 *  `Sandbox::resume()`); `"kill"` destroys the session. `autoResume` re-hydrates
 *  a paused session on the next re-attach. */
struct Lifecycle {
  /** "pause" or "kill". */
  std::string onTimeout;
  std::optional<bool> autoResume;
};

/** One attach-at-create instruction: a persistent volume and the absolute
 *  in-guest path to mount it at. The mount happens host-side before the guest
 *  starts, and survives pause/resume and recreate. */
struct VolumeAttachment {
  /** A `vol_…` id belonging to your org. */
  std::string volumeId;
  /** Absolute in-guest mount point, e.g. `/data`. */
  std::string path;
};

/** Options for `POST /sandboxes`. */
struct CreateSandboxOptions {
  std::optional<std::string> template_;
  std::optional<int> cpu;
  std::optional<int> memMb;
  std::optional<int> diskGb;
  std::map<std::string, std::string> envs;
  std::map<std::string, std::string> metadata;
  std::optional<long> timeoutMs;
  std::optional<std::string> fromSnapshot;
  /** Idle lifecycle policy (pause/kill on timeout; optional auto-resume). */
  std::optional<Lifecycle> lifecycle;
  /** Initial display resolution, e.g. "1280x720". Desktops only — a headless
   *  sandbox has no display. */
  std::optional<std::string> resolution;
  /** Record the session server-side; the create response carries a presigned
   *  playback URL. Desktops only — `record` on a headless sandbox is rejected
   *  (400 `RecordingRequiresDesktop`). */
  std::optional<bool> record;
  /** Persistent volumes to mount before the session starts. */
  std::vector<VolumeAttachment> volumes;
};

/** `201` response from `POST /sandboxes`. */
struct CreateSandboxResponse {
  std::string sandboxId;
  std::string kind = "sandbox";
  std::string controlUrl;
  std::string expiresAt;
  std::optional<std::string> streamUrl;
};

/** `GET /sandboxes/:id` response. */
struct SandboxView {
  std::string sandboxId;
  std::string kind;
  std::string state;
  std::string expiresAt;
  std::optional<std::string> controlUrl;
};

} // namespace solari

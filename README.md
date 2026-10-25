# Solari C++ SDK (core surface)

A C++17 binding for the Solari sandbox gateway. It mirrors `@solarisdk/core` +
`@solarisdk/sandbox`: a `solari::Client` (create / connect / get / kill over
REST) hands back a `solari::Sandbox` handle exposing

- `.commands` — `run` / `start` / stdin / kill (streamed exec over the control WS)
- `.files` — `read` / `write` / `list` / `stat` / `mkdir` / `remove` / `rename`
- `.code` — `code.run` with matplotlib charts flattened out of the results
- `.git` — `clone` / `status` / `add` / `commit` / `push` / `pull` / `checkout`
  / `branches` / `log`, composed client-side over `commands.run("git", …)`

It behaves identically on the wire to the reference TypeScript
(`@solarisdk/core`) SDK — see [`PROTOCOL.md`](PROTOCOL.md) for the contract.

The API is **blocking and synchronous**. A background receive thread (inside the
IXWebSocket transport) feeds a newline-delimited-JSON frame dispatcher; RPC calls
block on a `std::future` until their correlated reply, and `commands.run` blocks
until the command's `cmd.exit` frame arrives.

Not included by design: pty, volumes, computer-use, viewer, snapshots, templates.

## Layout

```
include/solari/*.hpp   public headers (umbrella: solari/solari.hpp)
src/*.cpp              implementation
tests/test_main.cpp    offline doctest suite (no gateway needed)
CMakeLists.txt         FetchContent deps + library + test target
```

## Dependencies

Pulled automatically via CMake `FetchContent`:

- [nlohmann/json](https://github.com/nlohmann/json) `v3.11.3` — JSON
- [IXWebSocket](https://github.com/machinezone/IXWebSocket) `v11.4.5` — control
  WebSocket, TLS via OpenSSL (`USE_TLS=ON`, `USE_OPEN_SSL=ON`)
- [doctest](https://github.com/doctest/doctest) `v2.4.11` — tests

Required from the system:

- **libcurl** (`find_package(CURL REQUIRED)`) — REST transport
- a threads library (`find_package(Threads REQUIRED)`)
- OpenSSL — for IXWebSocket TLS

On Debian/Ubuntu: `sudo apt-get install libcurl4-openssl-dev libssl-dev`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Options:

- `-DSOLARI_WITH_WS=OFF` — skip IXWebSocket. The library still builds; the frame
  dispatcher, REST client, `code.run` flattening and all of `git` are fully
  usable and tested, but `Sandbox::connect()` throws `ConnectionError` because
  there is no live control transport.
- `-DSOLARI_BUILD_TESTS=OFF` — skip the test target.

Link against the `solari` target (or `libsolari.a` + its headers).

## Example: create → run a command → git

```cpp
#include <iostream>
#include <solari/solari.hpp>

int main() {
  solari::ClientOptions opts;
  opts.apiKey  = "slr_live_...";
  opts.baseUrl = "https://api.getsolari.com";
  solari::Client client(opts);

  // Create a sandbox and open its control channel.
  auto sbx = client.create();          // POST /sandboxes
  sbx->connect();                      // control WebSocket (needs SOLARI_WITH_WS)

  // Run a command to completion.
  solari::CommandOptions o;
  o.args = {"-la"};
  solari::CommandResult r = sbx->commands.run("ls", o);
  std::cout << "exit " << r.exitCode << "\n" << r.stdout_;

  // Stream output from a longer command.
  auto h = sbx->commands.start("bash", [] {
    solari::CommandOptions bo;
    bo.args = {"-lc", "for i in 1 2 3; do echo line $i; sleep 1; done"};
    bo.onStdout = [](const std::string& s) { std::cout << s; };
    return bo;
  }());
  int code = h.wait();

  // Files.
  sbx->files.write("/tmp/hello.txt", "hi\n");
  std::string body = sbx->files.read("/tmp/hello.txt");
  for (const auto& e : sbx->files.list("/tmp"))
    std::cout << (e.dir ? "d " : "- ") << e.name << " (" << e.size << ")\n";

  // Run code and collect charts.
  auto rc = sbx->code.run("import matplotlib.pyplot as plt; plt.plot([1,2,3])",
                          [] { solari::RunCodeOptions co; co.language = "python"; return co; }());
  std::cout << rc.charts.size() << " chart(s)\n";

  // git: clone, edit, commit, push (auth spliced into the https remote).
  solari::GitCloneOptions clone;
  clone.path = "/work/repo";
  clone.username = "git-user";
  clone.password = "ghp_token";
  sbx->git.clone("https://github.com/acme/widgets.git", clone);

  sbx->files.write("/work/repo/NOTES.md", "updated by solari\n");
  sbx->git.add({"NOTES.md"}, std::string("/work/repo"));

  solari::GitCommitOptions ci;
  ci.cwd = "/work/repo";
  ci.author = "CI Bot";
  ci.email  = "ci@acme.dev";
  auto commit = sbx->git.commit("chore: update notes", ci);
  std::cout << "committed " << commit.hash << "\n";

  solari::GitRemoteOptions push;
  push.cwd = "/work/repo";
  push.username = "git-user";
  push.password = "ghp_token";
  sbx->git.push(push);

  solari::GitStatus st = sbx->git.status(std::string("/work/repo"));
  std::cout << "on " << st.branch << (st.clean ? " (clean)\n" : " (dirty)\n");

  sbx->kill();   // DELETE /sandboxes/:id + close the channel
  return 0;
}
```

## Errors

All failures throw exceptions derived from `solari::Error` (which is a
`std::runtime_error`):

- `GatewayError` (with `status` / `code` / `body`) and its typed subclasses
  `AuthError` (401), `PlanError` (402, or 403 `NotEntitled`), `ConcurrencyLimitError` (429 `ConcurrencyLimitExceeded`),
  `NoCapacityError` (503 `NoCapacity` / `StartTimeout`); other 403/429/503 codes are a plain `GatewayError`
- `ActionError` (an RPC replied `ok:false`), `TimeoutError`, `ConnectionError`

## Testing

`tests/test_main.cpp` runs fully offline (no gateway, no network). The
`ControlChannel` is transport-agnostic — tests feed it raw byte chunks via
`onMessage()` and capture outbound frames via `setSend()` — and `GitOps` runs
against an injected `CommandRunner` stub. Coverage includes newline-JSON frame
splitting (including a frame split across two reads and two frames in one
buffer), orphan async-frame buffering + flush, `commands.run` result assembly
from scripted frames, channel-close waking a frame-based exit-wait, the full git
parser/arg suite, `code.run` chart flattening, and the REST create/GET request
shapes.
```sh
ctest --test-dir build --output-on-failure   # 29 cases / 131 assertions
```

// code.run result parsing + the client-side chart-flatten convenience: every
// `results[i].chart` present is flattened into a top-level `charts` array.
// Ported from `@solarisdk/core` handle.ts runCode().
#pragma once
#include <functional>
#include <string>

#include "solari/types.hpp"

namespace solari {

/** Parse a raw `code.run` reply into RunCodeResult, flattening charts and
 *  streaming stdout/stderr text items to the callbacks (if provided). */
RunCodeResult flattenCodeResult(
    const Json& reply,
    const std::function<void(const std::string&)>& onStdout = nullptr,
    const std::function<void(const std::string&)>& onStderr = nullptr,
    const std::string& alreadyStdout = std::string(),
    const std::string& alreadyStderr = std::string());

/** Live output of one `code.run`: the v1 stream frames {id, stream, data(base64)} a streaming guest sends before the result.
 *  Delivers each to the callbacks as it arrives (a UTF-8 character split across two frames is held back and completed by the
 *  next one) and remembers what it delivered, so flattenCodeResult delivers only the rest of the result's text. */
class CodeStream {
 public:
  CodeStream(std::function<void(const std::string&)> onStdout, std::function<void(const std::string&)> onStderr);
  void onFrame(const Json& frame);
  const std::string& stdoutSent() const { return out_.sent; }
  const std::string& stderrSent() const { return err_.sent; }

 private:
  struct Side {
    std::string tail;  // an incomplete trailing UTF-8 sequence, held for the next frame
    std::string sent;  // everything delivered so far
  };
  void push(Side& side, const std::function<void(const std::string&)>& cb, const std::string& bytes);
  std::function<void(const std::string&)> onStdout_, onStderr_;
  Side out_, err_;
};

/** Parse one Chart object (elements kept raw). Exposed for testing. */
Chart parseChart(const Json& j);

} // namespace solari

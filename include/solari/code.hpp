// code.run result parsing + the client-side chart-flatten convenience: every
// `results[i].chart` present is flattened into a top-level `charts` array.
// Ported from `@solarisdk/core` handle.ts runCode().
#pragma once
#include <functional>

#include "solari/types.hpp"

namespace solari {

/** Parse a raw `code.run` reply into RunCodeResult, flattening charts and
 *  streaming stdout/stderr text items to the callbacks (if provided). */
RunCodeResult flattenCodeResult(
    const Json& reply,
    const std::function<void(const std::string&)>& onStdout = nullptr,
    const std::function<void(const std::string&)>& onStderr = nullptr);

/** Parse one Chart object (elements kept raw). Exposed for testing. */
Chart parseChart(const Json& j);

} // namespace solari

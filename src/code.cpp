#include "solari/code.hpp"

#include "solari/util.hpp"

namespace solari {

static std::optional<std::string> optStr(const Json& j, const char* k) {
  if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
  return std::nullopt;
}

static ChartAxis parseAxis(const Json& j) {
  ChartAxis a;
  if (!j.is_object()) return a;
  a.present = true;
  a.label = optStr(j, "label");
  a.scale = optStr(j, "scale");
  if (j.contains("ticks") && j["ticks"].is_array()) {
    for (const auto& t : j["ticks"]) a.ticks.push_back(t);
  }
  return a;
}

Chart parseChart(const Json& j) {
  Chart c;
  if (!j.is_object()) return c;
  c.type = optStr(j, "type").value_or("unknown");
  c.title = optStr(j, "title");
  c.xLabel = optStr(j, "xLabel");
  c.yLabel = optStr(j, "yLabel");
  if (j.contains("x")) c.x = parseAxis(j["x"]);
  if (j.contains("y")) c.y = parseAxis(j["y"]);
  if (j.contains("elements") && j["elements"].is_array()) c.elements = j["elements"];
  return c;
}

static CodeResultItem parseItem(const Json& j) {
  CodeResultItem it;
  it.type = optStr(j, "type").value_or("result");
  it.text = optStr(j, "text");
  it.png = optStr(j, "png");
  it.jpeg = optStr(j, "jpeg");
  it.svg = optStr(j, "svg");
  it.html = optStr(j, "html");
  it.latex = optStr(j, "latex");
  it.markdown = optStr(j, "markdown");
  if (j.contains("json")) it.json_ = j["json"];
  if (j.contains("chart") && !j["chart"].is_null()) it.chart = parseChart(j["chart"]);
  return it;
}

RunCodeResult flattenCodeResult(
    const Json& reply, const std::function<void(const std::string&)>& onStdout,
    const std::function<void(const std::string&)>& onStderr, const std::string& alreadyStdout,
    const std::string& alreadyStderr) {
  RunCodeResult out;
  std::string fullOut, fullErr;
  if (reply.contains("results") && reply["results"].is_array()) {
    for (const auto& r : reply["results"]) {
      CodeResultItem it = parseItem(r);
      if (it.type == "stdout" && it.text) fullOut += *it.text;
      if (it.type == "stderr" && it.text) fullErr += *it.text;
      if (it.chart) out.charts.push_back(*it.chart);
      out.results.push_back(std::move(it));
    }
  }
  // Deliver only what the stream frames did not: everything for a guest that predates streaming, the unflushed tail for one
  // that streams.
  auto rest = [](const std::string& full, const std::string& already) {
    if (already.empty()) return full;
    if (full.compare(0, already.size(), already) == 0) return full.substr(already.size());
    return std::string();
  };
  if (onStdout) {
    std::string r = rest(fullOut, alreadyStdout);
    if (!r.empty()) onStdout(r);
  }
  if (onStderr) {
    std::string r = rest(fullErr, alreadyStderr);
    if (!r.empty()) onStderr(r);
  }
  if (reply.contains("error") && !reply["error"].is_null()) out.error = reply["error"];
  return out;
}

CodeStream::CodeStream(std::function<void(const std::string&)> onStdout,
                       std::function<void(const std::string&)> onStderr)
    : onStdout_(std::move(onStdout)), onStderr_(std::move(onStderr)) {}

void CodeStream::push(Side& side, const std::function<void(const std::string&)>& cb, const std::string& bytes) {
  std::string b = side.tail + bytes;
  // Hold back an incomplete trailing UTF-8 sequence (a lead byte whose continuation bytes have not arrived yet).
  std::size_t cut = b.size();
  for (std::size_t i = 1; i <= 3 && i <= b.size(); i++) {
    unsigned char c = static_cast<unsigned char>(b[b.size() - i]);
    if ((c & 0xC0) == 0x80) continue;  // continuation byte: keep looking for its lead byte
    std::size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (need > i) cut = b.size() - i;
    break;
  }
  side.tail = b.substr(cut);
  std::string text = utf8Lossy(b.substr(0, cut));
  if (text.empty()) return;
  side.sent += text;
  if (cb) cb(text);
}

void CodeStream::onFrame(const Json& frame) {
  if (!frame.contains("stream") || !frame["stream"].is_string()) return;
  if (!frame.contains("data") || !frame["data"].is_string()) return;
  std::string bytes = base64Decode(frame["data"].get<std::string>());
  if (bytes.empty()) return;
  const std::string stream = frame["stream"].get<std::string>();
  if (stream == "stdout") push(out_, onStdout_, bytes);
  else if (stream == "stderr") push(err_, onStderr_, bytes);
}

} // namespace solari

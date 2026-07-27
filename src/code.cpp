#include "solari/code.hpp"

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
    const std::function<void(const std::string&)>& onStderr) {
  RunCodeResult out;
  if (reply.contains("results") && reply["results"].is_array()) {
    for (const auto& r : reply["results"]) {
      CodeResultItem it = parseItem(r);
      if (it.type == "stdout" && it.text && onStdout) onStdout(*it.text);
      if (it.type == "stderr" && it.text && onStderr) onStderr(*it.text);
      if (it.chart) out.charts.push_back(*it.chart);
      out.results.push_back(std::move(it));
    }
  }
  if (reply.contains("error") && !reply["error"].is_null()) out.error = reply["error"];
  return out;
}

} // namespace solari

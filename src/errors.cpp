#include "solari/errors.hpp"

namespace solari {

static std::string pickMessage(const nlohmann::json& body, int status) {
  if (body.is_object()) {
    for (const char* k : {"message", "error", "code"}) {
      if (body.contains(k) && body[k].is_string()) {
        return body[k].get<std::string>();
      }
    }
  }
  return "Gateway request failed with status " + std::to_string(status);
}

void throwGatewayError(int status, const nlohmann::json& body) {
  std::string msg = pickMessage(body, status);
  // `code` is the discriminator (PROTOCOL.md section 4); the status alone is not
  // enough because one status carries several meanings. A response with no code keeps
  // the type its status has always had.
  std::string code;
  if (body.is_object() && body.contains("code") && body["code"].is_string()) {
    code = body["code"].get<std::string>();
  }
  switch (status) {
    case 401:
      throw AuthError(msg, body);
    case 402:
      throw PlanError(msg, body);
    case 403:
      // The gateway's only JSON 403 is NotEntitled (the plan does not include sandboxes/desktops).
      if (code == "NotEntitled") throw PlanError(msg, body, 403);
      throw GatewayError(403, msg, body);
    case 429:
      if (code.empty() || code == "ConcurrencyLimitExceeded") throw ConcurrencyLimitError(msg, body);
      throw GatewayError(429, msg, body);
    case 503:
      if (code.empty() || code == "NoCapacity" || code == "StartTimeout") throw NoCapacityError(msg, body);
      throw GatewayError(503, msg, body);
    default:
      // body-level `no_capacity` also maps to NoCapacityError.
      if (code == "no_capacity") {
        throw NoCapacityError(msg, body);
      }
      throw GatewayError(status, msg, body);
  }
}

} // namespace solari

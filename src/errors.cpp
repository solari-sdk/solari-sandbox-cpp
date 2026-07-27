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
  switch (status) {
    case 401:
    case 403:
      throw AuthError(msg, body);
    case 402:
      throw PlanError(msg, body);
    case 429:
      throw ConcurrencyLimitError(msg, body);
    case 503:
      throw NoCapacityError(msg, body);
    default:
      // body-level `no_capacity` also maps to NoCapacityError.
      if (body.is_object() && body.contains("code") && body["code"].is_string() &&
          body["code"].get<std::string>() == "no_capacity") {
        throw NoCapacityError(msg, body);
      }
      throw GatewayError(status, msg, body);
  }
}

} // namespace solari

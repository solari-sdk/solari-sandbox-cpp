#include "solari/util.hpp"

namespace solari {

static const char* kB64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::string& bytes) {
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  std::size_t i = 0;
  const std::size_t n = bytes.size();
  while (i + 3 <= n) {
    std::uint32_t v = (static_cast<unsigned char>(bytes[i]) << 16) |
                      (static_cast<unsigned char>(bytes[i + 1]) << 8) |
                      (static_cast<unsigned char>(bytes[i + 2]));
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back(kB64[(v >> 6) & 0x3F]);
    out.push_back(kB64[v & 0x3F]);
    i += 3;
  }
  if (n - i == 1) {
    std::uint32_t v = static_cast<unsigned char>(bytes[i]) << 16;
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back('=');
    out.push_back('=');
  } else if (n - i == 2) {
    std::uint32_t v = (static_cast<unsigned char>(bytes[i]) << 16) |
                      (static_cast<unsigned char>(bytes[i + 1]) << 8);
    out.push_back(kB64[(v >> 18) & 0x3F]);
    out.push_back(kB64[(v >> 12) & 0x3F]);
    out.push_back(kB64[(v >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
  return base64Encode(std::string(bytes.begin(), bytes.end()));
}

std::string base64Decode(const std::string& b64) {
  static int T[256];
  static bool init = false;
  if (!init) {
    for (int i = 0; i < 256; i++) T[i] = -1;
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(kB64[i])] = i;
    init = true;
  }
  std::string out;
  int val = 0, bits = 0;
  for (unsigned char c : b64) {
    if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    int d = T[c];
    if (d < 0) continue;  // ignore stray non-alphabet bytes
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<char>((val >> bits) & 0xFF));
    }
  }
  return out;
}

std::string encodeURIComponent(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '!' || c == '~' || c == '*' ||
                      c == '\'' || c == '(' || c == ')';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  auto ws = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
  };
  while (b < e && ws(s[b])) b++;
  while (e > b && ws(s[e - 1])) e--;
  return s.substr(b, e - b);
}

} // namespace solari

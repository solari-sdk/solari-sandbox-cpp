// Small standalone helpers: base64, JS-compatible URL encoding, string trim.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace solari {

/** Base64-encode raw bytes (RFC 4648, with padding). */
std::string base64Encode(const std::string& bytes);
std::string base64Encode(const std::vector<std::uint8_t>& bytes);

/** Base64-decode; ignores whitespace, tolerant of missing padding. */
std::string base64Decode(const std::string& b64);

/**
 * Percent-encode exactly like JavaScript's `encodeURIComponent`: every char is
 * escaped except the "unreserved" set `A-Za-z0-9` and `-_.!~*'()`. This is what
 * the reference SDK relies on when splicing basic-auth creds into a remote URL
 * (`p@ss word` -> `p%40ss%20word`).
 */
std::string encodeURIComponent(const std::string& s);

/** Trim leading/trailing ASCII whitespace (matches JS `String.prototype.trim`). */
std::string trim(const std::string& s);

} // namespace solari

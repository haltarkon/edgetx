// SPDX-License-Identifier: GPL-2.0-or-later
//
// A few helpers to build JSON text in the headless mixer's glue.

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>

namespace etx {

// Append `s` (at most `n` bytes, stopping at NUL) as a JSON string literal.
//
// EdgeTX draws its icons and arrows with private glyph codes in the C1 block
// (translations/untranslated.h: CHAR_UP is "\302\202", U+0082, and so on).
// Arrows become their Unicode equivalents; icons are dropped, since the caller
// reports them separately (see glyphIcon()).
void appendJsonString(std::string& out, const char* s, size_t n = (size_t)-1);

// The icon an EdgeTX label starts with (CHAR_STICK, CHAR_POT, ...), as a word
// ("stick", "pot", ...), or nullptr when it starts with none.
const char* glyphIcon(const char* s);

inline void appendKey(std::string& out, const char* key)
{
  appendJsonString(out, key);
  out.push_back(':');
}

inline void appendInt(std::string& out, long long v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%lld", v);
  out += buf;
}

inline void appendBool(std::string& out, bool v) { out += v ? "true" : "false"; }

// Copy a generated string into the caller's buffer: returns the full length
// (excluding the terminating NUL), writes at most `max` bytes and a NUL when
// it fits. A result larger than `max` means "call again with that much".
inline int32_t copyOut(const std::string& s, char* dst, int32_t max)
{
  if (dst && max > 0) {
    size_t n = s.size() < (size_t)max ? s.size() : (size_t)max;
    memcpy(dst, s.data(), n);
    if (n < (size_t)max) dst[n] = '\0';
  }
  return (int32_t)s.size();
}

}  // namespace etx

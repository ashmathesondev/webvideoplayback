#pragma once

// Shared string helpers used by both executables.
//
// These functions are intentionally small and dependency-free. They cover the
// parsing and normalization needed by command-line handling, HTTP headers, and
// Windows path conversion.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <string>
#endif

namespace webvideoplayback::utils {

// Returns a lower-case copy using byte-wise ASCII-compatible conversion.
std::string lower_copy(std::string value);

// Trims leading and trailing ASCII whitespace.
std::string trim(std::string_view value);

// Parses an unsigned 64-bit integer without accepting partial input.
std::optional<std::uint64_t> parse_u64(std::string_view value);

// Decodes percent-escaped URL paths and maps '+' to a space.
std::string url_decode(std::string_view value);

#ifdef _WIN32
// Converts UTF-16 Windows strings to UTF-8 for FFmpeg and reports.
std::string wide_to_utf8(const std::wstring& value);
std::wstring utf8_to_wide(const std::string& value);
#endif

} // namespace webvideoplayback::utils

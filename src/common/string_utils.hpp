#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <string>
#endif

namespace webvideoplayback::utils {

std::string lower_copy(std::string value);
std::string trim(std::string_view value);
std::optional<std::uint64_t> parse_u64(std::string_view value);
std::string url_decode(std::string_view value);

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& value);
#endif

} // namespace webvideoplayback::utils

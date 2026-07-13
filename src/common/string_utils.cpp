#include "common/string_utils.hpp"

// Implementation notes:
// - The server only needs byte-wise HTTP token handling.
// - Windows paths are converted through Win32 APIs to preserve Unicode names.

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace webvideoplayback::utils {

std::string lower_copy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

std::optional<std::uint64_t> parse_u64(std::string_view value)
{
    std::uint64_t number = 0;
    const char* first = value.data();
    const char* last = value.data() + value.size();
    const auto result = std::from_chars(first, last, number);
    if (result.ec != std::errc() || result.ptr != last) {
        return std::nullopt;
    }
    return number;
}

std::string url_decode(std::string_view value)
{
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string_view hex = value.substr(i + 1, 2);
            unsigned int byte = 0;
            std::istringstream stream{std::string(hex)};
            stream >> std::hex >> byte;
            if (!stream.fail()) {
                decoded.push_back(static_cast<char>(byte));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

#ifdef _WIN32
std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("failed to convert path to UTF-8");
    }

    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("failed to convert path from UTF-8");
    }

    std::wstring result(static_cast<std::size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}
#endif

} // namespace webvideoplayback::utils

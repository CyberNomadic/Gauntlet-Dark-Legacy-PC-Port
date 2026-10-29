#include "engine/platform/Paths.h"

#include <string>
#include <string_view>

#ifdef _WIN32
#include <array>

#include <windows.h> // IWYU pragma: keep
#else
#include <cstdlib>
#include <system_error>
#endif

namespace gdl::paths {

std::filesystem::path executableDirectory() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH * 4> buffer{};
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
#else
    std::error_code ec;
    auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::filesystem::current_path();
    }
    return exe.parent_path();
#endif
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::string genericUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path fromUtf8(std::string_view text) {
    return {std::u8string(text.begin(), text.end())};
}

std::filesystem::path environment(const char* variable) {
#ifdef _WIN32
    const std::string_view narrow{variable};
    const std::wstring name(narrow.begin(), narrow.end()); // the names asked for are ASCII
    std::wstring value(MAX_PATH, L'\0');
    DWORD length =
        GetEnvironmentVariableW(name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    if (length >= value.size()) {
        value.resize(length); // the size needed, terminator included
        length =
            GetEnvironmentVariableW(name.c_str(), value.data(), static_cast<DWORD>(value.size()));
    }
    if (length == 0) {
        return {};
    }
    value.resize(length);
    return {value};
#else
    const char* value = std::getenv(variable); // NOLINT(concurrency-mt-unsafe): read at startup
    return value == nullptr ? std::filesystem::path{} : fromUtf8(value);
#endif
}

} // namespace gdl::paths

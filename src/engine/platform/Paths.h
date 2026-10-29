#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace gdl::paths {

/** Directory containing the running executable. */
std::filesystem::path executableDirectory();

/** The path as UTF-8 text, for messages, logs and names. Never throws: unlike
 * path::string() on Windows, which uses the system code page, every character has a
 * mapping. */
std::string utf8(const std::filesystem::path& path);
/** The same with forward slashes, for keys that must read alike on every platform. */
std::string genericUtf8(const std::filesystem::path& path);
/** A path from UTF-8 text (settings, catalogues, command lines), whatever the system
 * code page. */
std::filesystem::path fromUtf8(std::string_view text);
/** An environment variable as a path, read wide on Windows so a user name beyond the
 * system code page comes through whole; empty when unset. */
std::filesystem::path environment(const char* variable);

} // namespace gdl::paths

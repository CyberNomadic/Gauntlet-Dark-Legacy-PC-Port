#include "engine/render/opengl/OpenGlShaderSource.h"

#include <string>
#include <string_view>

#include "engine/core/Types.h"
#include "engine/io/File.h"

namespace gdl {

namespace {

constexpr std::string_view kVersionDirective = "#version";
constexpr std::string_view kIncludeExtension = "#extension GL_GOOGLE_include_directive";
constexpr std::string_view kIncludeDirective = "#include";

/** `text` without the blanks either side. */
std::string_view trimmed(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/** The name between the quotes of an include line, or empty. */
std::string_view includedName(std::string_view line) {
    const usize open = line.find('"');
    if (open == std::string_view::npos) {
        return {};
    }
    const usize close = line.find('"', open + 1);
    if (close == std::string_view::npos) {
        return {};
    }
    return line.substr(open + 1, close - open - 1);
}

/** Appends `file`'s lines, reading its includes in; only the entry file carries the
 * version line that is replaced. */
void append(std::string& out, const std::filesystem::path& file,
            std::span<const std::string> defines, bool entry) {
    const std::string text = readTextFile(file);
    std::string_view rest = text;
    while (!rest.empty()) {
        const usize end = rest.find('\n');
        const std::string_view raw =
            rest.substr(0, end == std::string_view::npos ? rest.size() : end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        const std::string_view line = trimmed(raw);
        if (entry && line.starts_with(kVersionDirective)) {
            out += kOpenGlShaderVersion;
            out += "\n#define GDL_OPENGL 1\n";
            for (const std::string& define : defines) {
                out += "#define ";
                out += define;
                out += '\n';
            }
            continue;
        }
        if (line.starts_with(kIncludeExtension)) {
            continue;
        }
        if (line.starts_with(kIncludeDirective)) {
            const std::string_view name = includedName(line);
            if (!name.empty()) {
                append(out, file.parent_path() / std::filesystem::path(name), defines, false);
                continue;
            }
        }
        out += line;
        out += '\n';
    }
}

} // namespace

std::string openGlShaderSource(const std::filesystem::path& file,
                               std::span<const std::string> defines) {
    std::string out;
    append(out, file, defines, true);
    return out;
}

} // namespace gdl

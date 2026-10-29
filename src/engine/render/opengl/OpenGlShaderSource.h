#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace gdl {

/** The GLSL the OpenGL device hands its compiler is the same source the Vulkan build
 * compiles to SPIR-V, read at run time: the `#version` line becomes OpenGL 3.3's, with
 * `GDL_OPENGL` and each of `defines` ("NAME value") defined after it, every `#include`
 * is read in from beside the file, and the directive that asked for includes is left
 * out. Throws FileError when a file cannot be read. */
std::string openGlShaderSource(const std::filesystem::path& file,
                               std::span<const std::string> defines = {});

/** The version line the device compiles against. */
inline constexpr std::string_view kOpenGlShaderVersion = "#version 330 core";

} // namespace gdl

#include <array>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "engine/core/Error.h"
#include "engine/io/File.h"
#include "engine/render/opengl/OpenGlShaderSource.h"

#include "TestSupport.h"

namespace {

using namespace gdl;

TEST_CASE("the OpenGL shader text is the Vulkan source with its version, defines and includes "
          "resolved",
          "[render][opengl][shaders]") {
    const auto dir = test::scratchDirectory("opengl-shader-source");
    writeTextFile(dir / "platform.glsl", "#ifdef GDL_OPENGL\n#define VARYING(slot)\n#endif\n");
    writeTextFile(dir / "body.glsl", "#include \"platform.glsl\"\nvoid body() {}\n");
    writeTextFile(dir / "entry.frag", "#version 450\r\n"
                                      "#extension GL_GOOGLE_include_directive : require\r\n"
                                      "#include \"body.glsl\"\r\n"
                                      "VARYING(0) out vec4 color;\r\n"
                                      "void main() { color = vec4(1.0); }\r\n");
    const std::array<std::string, 2> defines{"GDL_ALPHA_TO_COVERAGE 1", "MULTISAMPLED_DEPTH"};
    const std::string source = openGlShaderSource(dir / "entry.frag", defines);
    CHECK(source == "#version 330 core\n"
                    "#define GDL_OPENGL 1\n"
                    "#define GDL_ALPHA_TO_COVERAGE 1\n"
                    "#define MULTISAMPLED_DEPTH\n"
                    "#ifdef GDL_OPENGL\n"
                    "#define VARYING(slot)\n"
                    "#endif\n"
                    "void body() {}\n"
                    "VARYING(0) out vec4 color;\n"
                    "void main() { color = vec4(1.0); }\n");
    // Without defines only the platform is declared; a missing file is reported.
    const std::string plain = openGlShaderSource(dir / "entry.frag");
    CHECK(plain.starts_with("#version 330 core\n#define GDL_OPENGL 1\n#ifdef GDL_OPENGL\n"));
    CHECK_THROWS_AS(openGlShaderSource(dir / "missing.frag"), FileError);
    writeTextFile(dir / "broken.frag", "#version 450\n#include \"missing.glsl\"\n");
    CHECK_THROWS_AS(openGlShaderSource(dir / "broken.frag"), FileError);
}

TEST_CASE("the shipped shaders read as one OpenGL 3.3 source each, the prelude and the "
          "constants read in",
          "[render][opengl][shaders]") {
    const auto shaders = test::dataDirectory().parent_path() / "shaders";
    if (!std::filesystem::exists(shaders / "immediate.frag")) {
        SKIP("the shader sources are not available");
    }
    for (const char* name : {"immediate.vert", "immediate.frag"}) {
        CAPTURE(name);
        const std::array<std::string, 1> defines{"GDL_ALPHA_TO_COVERAGE 0"};
        const std::string source = openGlShaderSource(shaders / name, defines);
        CHECK(source.starts_with("#version 330 core\n#define GDL_OPENGL 1\n"));
        CHECK(source.find("#include") == std::string::npos);
        CHECK(source.find("GL_GOOGLE_include_directive") == std::string::npos);
        CHECK(source.find("#define GDL_ALPHA_TO_COVERAGE 0\n") != std::string::npos);
        // The prelude's OpenGL spellings and the shared constants precede the stage's code.
        CHECK(source.find("#define PUSH_CONSTANTS_END(block, name) }; uniform block name;") <
              source.find("PUSH_CONSTANTS_BEGIN(PushConstants)"));
        CHECK(source.find("PUSH_CONSTANTS_END(PushConstants, pc)") < source.find("void main()"));
        CHECK(source.find("#version", 1) == std::string::npos); // the one version line
    }
    const std::string vertex = openGlShaderSource(shaders / "immediate.vert");
    CHECK(vertex.find("gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;") != std::string::npos);
}

} // namespace

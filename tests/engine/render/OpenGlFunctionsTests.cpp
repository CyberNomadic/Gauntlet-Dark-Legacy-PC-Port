#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "engine/render/opengl/OpenGlFunctions.h"

namespace {

using namespace gdl;

void nothing() {}

TEST_CASE("the OpenGL entry points are looked up by their names and the first missing one is "
          "reported",
          "[render][opengl]") {
    gl::OpenGlFunctions functions;
    std::vector<std::string> asked;
    const auto everything = [&](const char* name) {
        asked.emplace_back(name);
        return static_cast<OpenGlProcedure>(&nothing);
    };
    CHECK(functions.load(everything).empty());
    CHECK(asked.size() > 60);
    CHECK(asked.front() == "glGetError");
    CHECK(functions.getError != nullptr);
    CHECK(functions.renderbufferStorageMultisample != nullptr);
    for (const std::string& name : asked) {
        CHECK(name.starts_with("gl"));
    }
    // A context short of one entry point names it, so the device can decline the API.
    gl::OpenGlFunctions partial;
    const auto without = [&](const char* name) {
        return std::string_view(name) == "glGenSamplers" ? nullptr
                                                         : static_cast<OpenGlProcedure>(&nothing);
    };
    CHECK(partial.load(without) == "glGenSamplers");
}

} // namespace

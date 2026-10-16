#pragma once

#include <catch2/catch_test_macros.hpp>

#include "FakeRenderDevice.h"

namespace gdl::test {

/** Independently loaded scenes have different resource addresses. Compare the
 * actual image on every texture stage, as well as all pipeline and vertex data. */
inline void checkTextureImage(const Texture* actual, const Texture* expected) {
    REQUIRE((actual == nullptr) == (expected == nullptr));
    if (actual == nullptr) {
        return;
    }
    CHECK(actual->width() == expected->width());
    CHECK(actual->height() == expected->height());
    const auto* a = dynamic_cast<const FakeTexture*>(actual);
    const auto* b = dynamic_cast<const FakeTexture*>(expected);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a->pixels == b->pixels);
}

inline void checkRecordedDraw(const RecordedDraw& actual, const RecordedDraw& expected) {
    checkTextureImage(actual.texture, expected.texture);
    checkTextureImage(actual.state.lightmap, expected.state.lightmap);
    checkTextureImage(actual.state.maskedTexture, expected.state.maskedTexture);
    checkTextureImage(actual.state.nextTexture, expected.state.nextTexture);
    auto a = actual.state;
    auto b = expected.state;
    a.lightmap = b.lightmap = nullptr;
    a.maskedTexture = b.maskedTexture = nullptr;
    a.nextTexture = b.nextTexture = nullptr;
    CHECK(a == b);
    CHECK(actual.transform == expected.transform);
    CHECK(actual.vertices == expected.vertices);
}

} // namespace gdl::test

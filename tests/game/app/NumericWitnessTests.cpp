#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "game/app/NumericWitness.h"

namespace {
using namespace gdl;
using namespace gdl::game;
} // namespace

TEST_CASE("the numeric witness is one value on every supported platform",
          "[native-online-identity][determinism][numeric-witness]") {
    // Recorded on Windows with MSVC and confirmed with GCC on Linux: the corpus is
    // portable math, square roots, remainders and basic operations only, so a change
    // here means one of those changed or the build fuses multiply-adds.
    constexpr std::string_view kExpected =
        "936caee0e639891522035b2182d85bf0bae7dfc84e8fb5cdcef9ff3c89b8c572";
    CHECK_NOTHROW(numeric_witness::prepareProcess());
    CHECK_NOTHROW(numeric_witness::requireModes());
    const std::string witness = numeric_witness::digest();
    CHECK(witness.size() == 64);
    CHECK(std::ranges::all_of(witness, [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0 &&
               std::isupper(static_cast<unsigned char>(c)) == 0;
    }));
    CHECK(witness == numeric_witness::digest());
    INFO(witness);
    CHECK(witness == kExpected);
}

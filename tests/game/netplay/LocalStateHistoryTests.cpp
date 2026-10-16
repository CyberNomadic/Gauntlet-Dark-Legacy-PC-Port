#include <limits>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "game/netplay/LocalStateHistory.h"

namespace {
using namespace gdl;
using namespace gdl::game;
struct Value {
    u64 frame = 0;
    std::vector<s32> values;
};

TEST_CASE("local history stores sixteen exact pre-tick values and rejects overwritten ticks",
          "[local-rewind][local-state-history]") {
    LocalStateHistory<Value> history;
    CHECK(history.find(0) == nullptr);
    for (u64 tick = 0; tick < 40; ++tick) {
        REQUIRE(history.capture(tick, [&](Value& value) {
            value.frame = tick;
            value.values = {1, static_cast<s32>(tick)};
            return true;
        }));
        for (u64 distance = 0; distance < LocalStateHistory<Value>::kCapacity && distance <= tick;
             ++distance) {
            const auto* found = history.find(tick - distance);
            REQUIRE(found);
            CHECK(found->frame == tick - distance);
            CHECK(found->values.back() == static_cast<s32>(tick - distance));
        }
        if (tick >= LocalStateHistory<Value>::kCapacity) {
            CHECK(history.find(tick - LocalStateHistory<Value>::kCapacity) == nullptr);
        }
    }
    history.discardAfter(31);
    CHECK(history.find(31) != nullptr);
    for (u64 tick = 32; tick < 40; ++tick) {
        CHECK(history.find(tick) == nullptr);
    }
    REQUIRE(history.capture(32, [](Value& value) {
        value.values = {99};
        return true;
    }));
    CHECK(history.find(32)->values == std::vector<s32>{99});
    history.clear();
    for (u64 tick = 0; tick < 40; ++tick) {
        CHECK(history.find(tick) == nullptr);
    }
}

TEST_CASE("failed local captures invalidate their slot instead of exposing a partial value",
          "[local-rewind][local-state-history]") {
    LocalStateHistory<Value> history;
    REQUIRE(history.capture(1, [](Value& value) {
        value.values = {1, 2};
        return true;
    }));
    CHECK_FALSE(history.capture(17, [](Value& value) {
        value.values[0] = 99;
        return false;
    }));
    CHECK(history.find(1) == nullptr);
    CHECK(history.find(17) == nullptr);
    REQUIRE(history.capture(1, [](Value& value) {
        value.values = {1, 2};
        return true;
    }));
    REQUIRE_THROWS_AS(history.capture(17,
                                      [](Value& value) -> bool {
                                          value.values[1] = 99;
                                          throw std::runtime_error("copy failed");
                                      }),
                      std::runtime_error);
    CHECK(history.find(1) == nullptr);
    CHECK(history.find(17) == nullptr);
}

TEST_CASE("history reuses allocated buffers and supports exact high tick identities",
          "[local-rewind][local-state-history]") {
    LocalStateHistory<Value> history;
    REQUIRE(history.capture(0, [](Value& value) {
        value.values.resize(1000);
        return true;
    }));
    const auto capacity = history.find(0)->values.capacity();
    history.clear();
    REQUIRE(history.capture(16, [capacity](Value& value) {
        CHECK(value.values.capacity() == capacity);
        value.values.assign(20, 42);
        return true;
    }));
    const auto last = std::numeric_limits<u64>::max();
    REQUIRE(history.capture(last, [last](Value& value) {
        value.frame = last;
        return true;
    }));
    REQUIRE(history.find(last));
    CHECK(history.find(last)->frame == last);
    CHECK(history.find(last - 16) == nullptr);
    history.discardAfter(last - 1);
    CHECK(history.find(last) == nullptr);
    CHECK(history.find(16) != nullptr);
}
} // namespace

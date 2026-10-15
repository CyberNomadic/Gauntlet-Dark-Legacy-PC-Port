#include <catch2/catch_test_macros.hpp>

#include "engine/app/Application.h"

namespace {
using namespace gdl;

// Constructing Application does not create a window or renderer. Clock policy
// can therefore be checked without taking over the desktop.
class TimingApplication final : public Application {
public:
    using Application::Application;
    using Application::maxFrameRate;
    using Application::setUpdateRate;
    using Application::updateRate;
};
} // namespace

TEST_CASE("online clock selection preserves the local presentation cap", "[app-timing]") {
    for (const u32 offline : {0U, 30U, 60U, 120U}) {
        ApplicationDesc desc;
        desc.updateRate = offline;
        desc.maxFrameRate = 30;
        TimingApplication app(desc);
        CHECK(app.updateRate() == offline);
        app.setUpdateRate(60);
        CHECK(app.updateRate() == 60);
        CHECK(app.maxFrameRate() == 30);
        app.setUpdateRate(60);
        CHECK(app.updateRate() == 60);
        app.setUpdateRate(offline);
        CHECK(app.updateRate() == offline);
        CHECK(app.maxFrameRate() == 30);
        CHECK(desc.updateRate == offline);
    }
}

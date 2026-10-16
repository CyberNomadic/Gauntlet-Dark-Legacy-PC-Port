#include <cerrno>
#include <cfenv>
#include <cmath>
#include <future>
#include <string>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "engine/assets/StringTable.h"
#include "engine/io/File.h"

#include "TestSupport.h"
#include "game/app/NativeOnlineIdentity.h"

#if defined(_WIN32) && defined(_MSC_VER)
#include <float.h>
#include <windows.h>
#endif
#if defined(_WIN32) && defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {
using namespace gdl;
using namespace gdl::game;

StringTable strings(std::string_view value = "first") {
    std::string numericError;
    const bool prepared = NativeOnlineIdentity::prepareProcessNumeric(&numericError);
    INFO(numericError);
    REQUIRE(prepared);
    const auto directory = test::scratchDirectory("native-online-identity");
    writeTextFile(directory / "en.json", "{\"cue\":\"" + std::string(value) + "\"}");
    StringTable result;
    REQUIRE(result.load(directory, "en"));
    return result;
}

struct FloatingEnvironment {
    std::fenv_t value{};
    FloatingEnvironment() { REQUIRE(std::fegetenv(&value) == 0); }
    ~FloatingEnvironment() { std::fesetenv(&value); }
};

struct ErrorNumber {
    int value = errno;
    ~ErrorNumber() { errno = value; }
};

TEST_CASE("native runtime profile is selected by actual compiler linkage",
          "[native-online-identity][determinism][native-runtime-linkage]") {
#if defined(_WIN32) && defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
    CHECK(NativeOnlineIdentity::runtimeProfile() == "windows-msvc-static-crt-v1");
#elif defined(_WIN32) && defined(_MSC_VER) && defined(_DLL)
    CHECK(NativeOnlineIdentity::runtimeProfile() == "windows-msvc-dynamic-crt-v1");
#elif defined(_WIN32)
    CHECK(NativeOnlineIdentity::runtimeProfile() == "windows-loaded-runtime-v1");
#elif defined(__linux__)
    CHECK(NativeOnlineIdentity::runtimeProfile() == "linux-loaded-runtime-v1");
#else
    CHECK(NativeOnlineIdentity::runtimeProfile() == "unsupported-runtime");
#endif
}

TEST_CASE("native online identity requires effective data and complete owned inputs",
          "[native-online-identity][determinism]") {
    std::string error;
    CHECK_FALSE(NativeOnlineIdentity::snapshot({}, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(NativeOnlineIdentity::capture({}, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(NativeOnlineIdentity::numericMatches({}));

    auto source = strings();
    const auto input = NativeOnlineIdentity::snapshot(source, &error);
    REQUIRE(input);
    CHECK(error.empty());
    CHECK(input->data == source.digest());
    CHECK(input->language == "en");
    CHECK(input->numeric.size() == 64);
    CHECK(NativeOnlineIdentity::numericMatches(*input));
    for (const int field : {0, 1, 2}) {
        auto bad = *input;
        if (field == 0) {
            bad.data[0] = 'g';
        } else if (field == 1) {
            bad.numeric.pop_back();
        } else {
            bad.language.clear();
        }
        CHECK_FALSE(NativeOnlineIdentity::capture(std::move(bad), &error));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("native identity snapshots own text independently from later table changes",
          "[native-online-identity][determinism]") {
    auto source = strings();
    const auto initial = NativeOnlineIdentity::snapshot(source);
    REQUIRE(initial);
    source = strings("second");
    const auto changed = NativeOnlineIdentity::snapshot(source);
    REQUIRE(changed);
    CHECK(initial->data != changed->data);
    CHECK(initial->numeric == changed->numeric);
    source = {};
    CHECK(initial->language == "en");
    CHECK(initial->data.size() == 64);
}

TEST_CASE("native runtime identity hashes the running image independently from text",
          "[native-online-identity][determinism]") {
    const auto initial = NativeOnlineIdentity::snapshot(strings());
    const auto changed = NativeOnlineIdentity::snapshot(strings("second"));
    REQUIRE(initial);
    REQUIRE(changed);
    std::string error;
    const auto first = NativeOnlineIdentity::capture(*initial, &error);
    INFO(error);
    REQUIRE(first);
    CHECK(error.empty());
    const auto second = NativeOnlineIdentity::capture(*changed, &error);
    INFO(error);
    REQUIRE(second);
    CHECK(error.empty());
    CHECK(first->runtime.size() == 64);
    CHECK(first->runtime == second->runtime);
    CHECK(first->data != second->data);
    CHECK(first->data == initial->data);
    CHECK(second->data == changed->data);
    CHECK(first->language == "en");
    CHECK(second->language == "en");
}

TEST_CASE("numeric identity detects rounding changes but ignores sticky exceptions",
          "[native-online-identity][determinism]") {
    FloatingEnvironment restore;
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    const auto source = strings();
    const auto nearest = NativeOnlineIdentity::snapshot(source);
    REQUIRE(nearest);
    ErrorNumber restoreErrno;
    errno = EDOM;
    REQUIRE(std::feraiseexcept(FE_INEXACT) == 0);
    const int exceptions = std::fetestexcept(FE_ALL_EXCEPT);
    const bool matches = NativeOnlineIdentity::numericMatches(*nearest);
    const int afterExceptions = std::fetestexcept(FE_ALL_EXCEPT);
    const int afterErrno = errno;
    CHECK(matches);
    CHECK(afterExceptions == exceptions);
    CHECK(afterErrno == EDOM);
    const auto sticky = NativeOnlineIdentity::snapshot(source);
    REQUIRE(sticky);
    CHECK(sticky->numeric == nearest->numeric);
    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    CHECK_FALSE(NativeOnlineIdentity::numericMatches(*nearest));
    const auto downward = NativeOnlineIdentity::snapshot(source);
#if defined(_WIN32) && defined(_M_X64)
    CHECK_FALSE(downward);
    std::string error;
    CHECK_FALSE(NativeOnlineIdentity::prepareProcessNumeric(&error));
    CHECK_FALSE(error.empty());
    CHECK(std::fegetround() == FE_DOWNWARD);
#else
    REQUIRE(downward);
    CHECK(downward->numeric != nearest->numeric);
#endif
    REQUIRE(std::fesetround(FE_TONEAREST) == 0);
    CHECK(NativeOnlineIdentity::numericMatches(*nearest));
}

TEST_CASE("prepared numeric identity agrees on a new thread without sharing live text",
          "[native-online-identity][determinism]") {
    const auto source = strings();
    const auto input = NativeOnlineIdentity::snapshot(source);
    REQUIRE(input);
    const auto copy = *input;
    auto worker = std::async(std::launch::async,
                             [copy] { return NativeOnlineIdentity::numericMatches(copy); });
    CHECK(worker.get());
    CHECK(NativeOnlineIdentity::prepareProcessNumeric());
    CHECK(NativeOnlineIdentity::numericMatches(*input));
}

#if defined(_WIN32) && defined(_MSC_VER)
TEST_CASE("native runtime linkage agrees with actual linked math and control addresses",
          "[native-online-identity][determinism][native-runtime-linkage]") {
    const HMODULE executable = GetModuleHandleW(nullptr);
    REQUIRE(executable != nullptr);
    const auto check = [executable](auto function) {
        HMODULE owner = nullptr;
        REQUIRE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(function), &owner) != 0);
#if defined(_MT) && !defined(_DLL)
        CHECK(owner == executable);
#elif defined(_DLL)
        CHECK(owner != executable);
#endif
    };
    check(static_cast<double (*)(double)>(&::sin));
    check(static_cast<double (*)(double, double)>(&::pow));
    check(&::_controlfp_s);
#if defined(_M_X64)
    check(&::_get_FMA3_enable);
#endif
    std::string error;
    const auto input = NativeOnlineIdentity::snapshot(strings(), &error);
    INFO(error);
    REQUIRE(input);
    const auto identity = NativeOnlineIdentity::capture(*input, &error);
    INFO(error);
    REQUIRE(identity);
}

#if defined(_MT) && !defined(_DLL)
TEST_CASE("static runtime identity ignores an actual incidental system UCRT",
          "[native-online-identity][determinism][native-runtime-linkage]") {
    const auto input = NativeOnlineIdentity::snapshot(strings());
    REQUIRE(input);
    std::string error;
    const auto before = NativeOnlineIdentity::capture(*input, &error);
    INFO(error);
    REQUIRE(before);
    {
        struct Module {
            HMODULE handle;
            ~Module() {
                if (handle != nullptr) FreeLibrary(handle);
            }
        } incidental{LoadLibraryExW(L"ucrtbase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
        REQUIRE(incidental.handle != nullptr);
        const auto externalSin = GetProcAddress(incidental.handle, "sin");
        REQUIRE(externalSin != nullptr);
        HMODULE externalOwner = nullptr;
        REQUIRE(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(externalSin), &externalOwner) != 0);
        // A real alternative math implementation is present, but it is not the
        // statically linked function whose provenance capture() verifies.
        CHECK(externalOwner != GetModuleHandleW(nullptr));
        const auto during = NativeOnlineIdentity::capture(*input, &error);
        INFO(error);
        REQUIRE(during);
        CHECK(during->runtime == before->runtime);
        CHECK(during->data == before->data);
        CHECK(NativeOnlineIdentity::numericMatches(*input));
    }
    const auto after = NativeOnlineIdentity::capture(*input, &error);
    INFO(error);
    REQUIRE(after);
    CHECK(after->runtime == before->runtime);
    CHECK(NativeOnlineIdentity::numericMatches(*input));
}
#endif
#endif

#if defined(_WIN32) && defined(_M_X64)
TEST_CASE("Windows numeric policy rejects flushing and exception-mask drift without repair",
          "[native-online-identity][determinism]") {
    const auto source = strings();
    const auto input = NativeOnlineIdentity::snapshot(source);
    REQUIRE(input);
    struct Restore {
        unsigned int value = _mm_getcsr();
        ~Restore() { _mm_setcsr(value); }
    } restore;
    for (const unsigned int mask : {0x8000U, 0x0040U, 0x0080U}) {
        CAPTURE(mask);
        _mm_setcsr((restore.value & ~0x003fU) ^ mask);
        std::string error;
        CHECK_FALSE(NativeOnlineIdentity::snapshot(source, &error));
        CHECK_FALSE(error.empty());
        CHECK_FALSE(NativeOnlineIdentity::numericMatches(*input));
        CHECK_FALSE(NativeOnlineIdentity::prepareProcessNumeric(&error));
        CHECK_FALSE(error.empty());
        CHECK((_mm_getcsr() & 0xffc0U) == ((restore.value ^ mask) & 0xffc0U));
        _mm_setcsr(restore.value);
        CHECK(NativeOnlineIdentity::numericMatches(*input));
    }
}

TEST_CASE("Windows numeric policy detects later CRT FMA changes instead of resetting them",
          "[native-online-identity][determinism]") {
    const auto source = strings();
    const auto input = NativeOnlineIdentity::snapshot(source);
    REQUIRE(input);
    CHECK(_get_FMA3_enable() == 0);
    struct Restore {
        ~Restore() { _set_FMA3_enable(0); }
    } restore;
    if (_set_FMA3_enable(1) == 0) {
        SUCCEED("This processor has no UCRT FMA3 path; startup still pins the disabled profile");
        return;
    }
    std::string error;
    CHECK_FALSE(NativeOnlineIdentity::snapshot(source, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(NativeOnlineIdentity::numericMatches(*input));
    CHECK_FALSE(NativeOnlineIdentity::prepareProcessNumeric(&error));
    CHECK_FALSE(error.empty());
    CHECK(_get_FMA3_enable() != 0);
    _set_FMA3_enable(0);
    CHECK(NativeOnlineIdentity::numericMatches(*input));
}
#endif

} // namespace

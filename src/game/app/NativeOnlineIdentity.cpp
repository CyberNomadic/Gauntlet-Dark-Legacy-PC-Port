#include "game/app/NativeOnlineIdentity.h"

#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include "engine/core/Types.h"
#include "engine/io/Sha256.h"

#include "game/app/NumericWitness.h"
#include "game/app/SourceIdentity.h"

namespace gdl::game {
namespace {

class Digest {
public:
    void word(u64 value) {
        std::array<u8, 8> bytes{};
        for (usize i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(value >> (8 * i));
        }
        m_hash.update(bytes);
    }
    void text(std::string_view value) {
        word(value.size());
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the bytes of the text
        m_hash.update({reinterpret_cast<const u8*>(value.data()), value.size()});
    }
    std::string finish() const { return m_hash.hex(); }

private:
    Sha256 m_hash;
};

void fail(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool digestText(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool languageText(std::string_view value) {
    return !value.empty() && value.size() <= 32 &&
           std::ranges::all_of(value, [](char c) { return c >= 32 && c <= 126; });
}

} // namespace

std::string_view NativeOnlineIdentity::runtimeProfile() {
#if defined(_WIN32) && defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
    return "windows-msvc-static-crt-v1";
#elif defined(_WIN32) && defined(_MSC_VER) && defined(_DLL)
    return "windows-msvc-dynamic-crt-v1";
#elif defined(_WIN32)
    return "windows-loaded-runtime-v1";
#elif defined(__linux__)
    return "linux-loaded-runtime-v1";
#else
    return "unsupported-runtime";
#endif
}

bool NativeOnlineIdentity::prepareProcessNumeric(std::string* error) {
    fail(error, {});
    try {
        numeric_witness::prepareProcess();
        numeric_witness::requireModes();
        return true;
    } catch (const std::exception& exception) {
        fail(error, exception.what());
        return false;
    }
}

std::optional<NativeOnlineIdentity::Inputs>
NativeOnlineIdentity::snapshot(const StringTable& strings, std::string* error) {
    fail(error, {});
    if (!strings.loaded() || !languageText(strings.language())) {
        fail(error, "native online identity requires a loaded effective language table");
        return std::nullopt;
    }
    try {
        return Inputs{strings.digest(), strings.language(), numeric_witness::digest()};
    } catch (const std::exception& exception) {
        fail(error, exception.what());
        return std::nullopt;
    }
}

std::optional<NativeOnlineIdentity::Result> NativeOnlineIdentity::capture(Inputs inputs,
                                                                          std::string* error) {
    fail(error, {});
    if (!digestText(inputs.data) || !digestText(inputs.numeric) || !languageText(inputs.language)) {
        fail(error, "native online identity input is incomplete or malformed");
        return std::nullopt;
    }
    if (!digestText(sourceIdentity())) {
        fail(error, "the build carries no source identity");
        return std::nullopt;
    }
    Digest digest;
    digest.text("gdl-native-runtime-identity-v3");
    digest.text("numeric");
    digest.text(inputs.numeric);
    digest.text("sources");
    digest.text(sourceIdentity());
    return Result{digest.finish(), std::move(inputs.data), std::move(inputs.language)};
}

bool NativeOnlineIdentity::numericMatches(const Inputs& inputs) {
    if (!digestText(inputs.numeric)) {
        return false;
    }
    try {
        return inputs.numeric == numeric_witness::digest();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace gdl::game

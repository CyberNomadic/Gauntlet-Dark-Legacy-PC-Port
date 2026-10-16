#include "game/app/NativeOnlineIdentity.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cfenv>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/Types.h"
#include "engine/io/Sha256.h"
#include "engine/math/Math.h"

#ifdef _WIN32
#include <float.h>
#include <windows.h> // IWYU pragma: keep
#if defined(_M_X64)
#include <xmmintrin.h>
#endif
#elif defined(__linux__)
#include <dlfcn.h>
#if defined(__x86_64__)
#include <cpuid.h>
#include <xmmintrin.h>
#endif
#endif

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

void platform(Digest& digest) {
#ifdef _WIN32
    digest.text("windows");
#elif defined(__linux__)
    digest.text("linux");
#else
    throw std::runtime_error("native online runtime identity is unavailable on this OS");
#endif
#if defined(_M_X64) || defined(__x86_64__)
    digest.text("x86-64");
#else
    throw std::runtime_error("native online numeric identity is unavailable on this architecture");
#endif
#ifdef __clang__
    digest.text("clang");
    digest.text(__clang_version__);
#elif defined(_MSC_VER)
    digest.text("msvc");
    digest.word(_MSC_FULL_VER);
    digest.word(_MSC_BUILD);
#elif defined(__GNUC__)
    digest.text("gcc");
    digest.word(__GNUC__);
    digest.word(__GNUC_MINOR__);
    digest.word(__GNUC_PATCHLEVEL__);
#else
    throw std::runtime_error("native online compiler identity is unavailable");
#endif
#ifdef _MSVC_STL_UPDATE
    digest.word(_MSVC_STL_UPDATE);
#elif defined(_LIBCPP_VERSION)
    digest.word(_LIBCPP_VERSION);
#elif defined(__GLIBCXX__)
    digest.word(__GLIBCXX__);
#else
    throw std::runtime_error("native online C++ library identity is unavailable");
#endif
    digest.word(sizeof(void*));
    digest.word(sizeof(long double));
    digest.word(std::numeric_limits<float>::digits);
    digest.word(std::numeric_limits<double>::digits);
    digest.word(std::numeric_limits<long double>::digits);
    digest.word(std::endian::native == std::endian::little ? 1 : 2);
}

#if !defined(_WIN32) || !defined(_M_X64)
std::array<u32, 4> cpu(u32 leaf, u32 subleaf = 0) {
    std::array<u32, 4> result{};
#if defined(__linux__) && defined(__x86_64__)
    if (__get_cpuid_count(leaf, subleaf, &result[0], &result[1], &result[2], &result[3]) == 0) {
        throw std::runtime_error("native online CPU feature identity is unavailable");
    }
#else
    (void)leaf;
    (void)subleaf;
    throw std::runtime_error("native online CPU feature identity is unavailable");
#endif
    return result;
}
#endif

#if defined(_WIN32) && defined(_M_X64)
unsigned int windowsControl() {
    unsigned int control = 0;
    if (_controlfp_s(&control, 0, 0) != 0) {
        throw std::runtime_error("cannot inspect native online floating-point control");
    }
    const auto sse = _mm_getcsr();
    if (std::fegetround() != FE_TONEAREST || (sse & 0x6000U) != 0 ||
        (control & _MCW_RC) != _RC_NEAR) {
        throw std::runtime_error("native online numeric profile requires round-to-nearest");
    }
    if ((sse & 0x8040U) != 0 || (control & _MCW_DN) != _DN_SAVE) {
        throw std::runtime_error("native online numeric profile requires gradual underflow");
    }
    if ((sse & 0x1f80U) != 0x1f80U || (control & _MCW_EM) != _MCW_EM) {
        throw std::runtime_error("native online numeric profile requires masked exceptions");
    }
    return control;
}

class PreserveArithmeticStatus {
public:
    PreserveArithmeticStatus() : m_errno(errno) {
        if (std::fegetenv(&m_environment) != 0) {
            throw std::runtime_error("cannot preserve native online arithmetic status");
        }
    }
    ~PreserveArithmeticStatus() {
        std::fesetenv(&m_environment);
        errno = m_errno;
    }

private:
    int m_errno;
    std::fenv_t m_environment{};
};

// Volatile loads keep the witness as actual operations in this executable,
// rather than compiler-evaluated constants. Every emitted result is hashed by
// its IEEE bits, including signed zero; there is no tolerance or quantization.
f32 operand(u32 bits) {
    const volatile u32 stored = bits;
    return std::bit_cast<f32>(static_cast<u32>(stored));
}

f64 operand(u64 bits) {
    const volatile u64 stored = bits;
    return std::bit_cast<f64>(static_cast<u64>(stored));
}

void result(Digest& digest, f32 value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("native numeric corpus produced a nonfinite result");
    }
    digest.word(std::bit_cast<u32>(value));
}

void result(Digest& digest, f64 value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("native numeric corpus produced a nonfinite result");
    }
    digest.word(std::bit_cast<u64>(value));
}

void corpus(Digest& digest) {
    PreserveArithmeticStatus restore;
    digest.text("gdl-native-operation-corpus-v1");
    // Player headings/movement, tree rotations, damping and collision algebra.
    // Include angle boundaries, adjacent unit values and gradual underflow.
    constexpr u32 singles[] = {0x00000000U, 0x80000000U, 0x00000001U, 0x00800000U, 0x3eaaaaabU,
                               0xbeaaaaabU, 0x3f7fffffU, 0x3f800000U, 0x3f800001U, 0xbf800001U,
                               0x3fc90fdaU, 0x3fc90fdbU, 0x3fc90fdcU, 0x40490fdaU, 0x40490fdbU,
                               0xc0490fdbU, 0x40c90fdbU, 0x447a0001U, 0xc47a0001U};
    for (const auto bits : singles) {
        const f32 value = operand(bits);
        const f32 positive = std::abs(value) + operand(u32{0x3e000000});
        result(digest, std::sin(value));
        result(digest, std::cos(value));
        result(digest, std::tan(value));
        result(digest, std::atan2(value, operand(u32{0xbf000001})));
        result(digest, std::asin(std::clamp(value, -1.0f, 1.0f)));
        result(digest, std::acos(std::clamp(value, -1.0f, 1.0f)));
        result(digest, std::sqrt(std::abs(value)));
        result(digest, std::hypot(value, operand(u32{0x3f000001})));
        result(digest, std::pow(positive, operand(u32{0x3f555555})));
        result(digest, std::fmod(value, operand(u32{0x3fc90fdb})));
        result(digest, std::exp(std::clamp(value, -8.0f, 8.0f)));
        result(digest, std::log(positive));
        result(digest, value * operand(u32{0x3f000000}));
        result(digest, value / operand(u32{0x40400000}));
        result(digest, (value + positive) - positive);
    }
    constexpr u64 doubles[] = {
        0x0000000000000000ULL, 0x8000000000000000ULL, 0x0000000000000001ULL, 0x0010000000000000ULL,
        0x3fd5555555555555ULL, 0xbfd5555555555555ULL, 0x3fefffffffffffffULL, 0x3ff0000000000000ULL,
        0x3ff0000000000001ULL, 0xbff0000000000001ULL, 0x3ff921fb54442d17ULL, 0x3ff921fb54442d18ULL,
        0x3ff921fb54442d19ULL, 0x400921fb54442d18ULL, 0xc00921fb54442d18ULL, 0x408f400000000001ULL};
    for (const auto bits : doubles) {
        const f64 value = operand(bits);
        const f64 positive = std::abs(value) + operand(u64{0x3fc0000000000000ULL});
        result(digest, std::sin(value));
        result(digest, std::cos(value));
        result(digest, std::tan(value));
        result(digest, std::atan2(value, operand(u64{0xbfe0000000000001ULL})));
        result(digest, std::asin(std::clamp(value, -1.0, 1.0)));
        result(digest, std::acos(std::clamp(value, -1.0, 1.0)));
        result(digest, std::sqrt(std::abs(value)));
        result(digest, std::hypot(value, operand(u64{0x3fe0000000000001ULL})));
        result(digest, std::pow(positive, operand(u64{0x3feaaaaaaaaaaaabULL})));
        result(digest, std::fmod(value, operand(u64{0x3ff921fb54442d18ULL})));
        result(digest, std::exp(std::clamp(value, -8.0, 8.0)));
        result(digest, std::log(positive));
        result(digest, value * operand(u64{0x3fe0000000000000ULL}));
        result(digest, value / operand(u64{0x4008000000000000ULL}));
        result(digest, (value + positive) - positive);
    }
    const f32 a = operand(u32{0x3f800001});
    const f32 b = operand(u32{0x3f7ffffe});
    const f32 c = operand(u32{0xbf800000});
    const volatile f32 product = a * b;
    result(digest, a * b + c);
    result(digest, static_cast<f32>(product) + c);
    const f64 da = operand(u64{0x3ff0000000000001ULL});
    const f64 db = operand(u64{0x3feffffffffffffeULL});
    const f64 dc = operand(u64{0xbff0000000000000ULL});
    const volatile f64 doubleProduct = da * db;
    result(digest, da * db + dc);
    result(digest, static_cast<f64>(doubleProduct) + dc);

    for (const auto bits : {u32{0x3eaaaaab}, u32{0xbf800001}, u32{0x40490fdb}}) {
        const f32 angle = operand(bits);
        const Vec3 from{angle, a, b};
        const Vec3 to{b, angle, a};
        const Vec3 axis = glm::normalize(from);
        const Vec3 cross = glm::cross(from, to);
        const Quat rotation = glm::angleAxis(angle, axis);
        const Quat blended = glm::slerp(rotation, glm::angleAxis(-angle, glm::normalize(to)),
                                        operand(u32{0x3eaaaaab}));
        const Mat4 matrix = glm::scale(glm::translate(Mat4{1.0f}, from) * glm::mat4_cast(rotation),
                                       Vec3{a, b, operand(u32{0x3f000001})});
        const Mat4 inverse = glm::inverse(matrix);
        const Vec4 point = inverse * (matrix * Vec4{to, 1.0f});
        for (s32 i = 0; i < 3; ++i) {
            result(digest, axis[i]);
            result(digest, cross[i]);
        }
        result(digest, glm::dot(from, to));
        for (s32 i = 0; i < 4; ++i) {
            result(digest, blended[i]);
            result(digest, point[i]);
            for (s32 j = 0; j < 4; ++j) {
                result(digest, matrix[i][j]);
                result(digest, inverse[i][j]);
            }
        }
    }
}
#endif

std::string numeric() {
    Digest digest;
    digest.text("gdl-native-numeric-mode-v2");
    platform(digest);
#if defined(_WIN32) && defined(_M_X64)
    const auto control = windowsControl();
    if (_get_FMA3_enable() != 0) {
        throw std::runtime_error("native online numeric profile requires startup FMA3 disable");
    }
    digest.text("ucrt-fma3-off-operation-profile-v1");
    digest.word(_mm_getcsr() & 0xffc0U);
    digest.word(control & (_MCW_DN | _MCW_EM | _MCW_IC | _MCW_RC | _MCW_PC));
    corpus(digest);
#else
    const int rounding = std::fegetround();
    if (rounding == -1) {
        throw std::runtime_error("cannot inspect native online rounding mode");
    }
    digest.word(static_cast<u32>(rounding));
#if defined(_M_X64) || defined(__x86_64__)
    // Only control/mask bits: arithmetic exception status changes during normal
    // play and must not make a caller's compatibility identity change.
    digest.word(_mm_getcsr() & 0xffc0U);
#endif
    // Linux libm dispatch has not been validated across CPUs. Retain its same
    // vendor/model/features restriction until a separate profile is established.
    // Exclude processor/APIC IDs and logical-processor count from leaf 1 EBX.
    const auto vendor = cpu(0);
    digest.word(vendor[1]);
    digest.word(vendor[2]);
    digest.word(vendor[3]);
    if (vendor[0] < 1) {
        throw std::runtime_error("native online CPU feature identity is incomplete");
    }
    const auto features = cpu(1);
    digest.word(features[0]);
    digest.word(features[2]);
    digest.word(features[3]);
    digest.word(vendor[0] >= 7 ? 1 : 0);
    if (vendor[0] >= 7) {
        const auto extended = cpu(7);
        digest.word(extended[1]);
        digest.word(extended[2]);
        digest.word(extended[3]);
    }
#endif
    return digest.finish();
}

std::string fileDigest(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    const auto time = std::filesystem::last_write_time(path);
    if (size == 0) {
        throw std::runtime_error("native runtime image is empty");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open native runtime image");
    }
    Sha256 hash;
    std::array<u8, 65536> bytes{};
    u64 total = 0;
    while (file) {
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        const auto count = file.gcount();
        if (count < 0) {
            throw std::runtime_error("cannot read native runtime image");
        }
        hash.update({bytes.data(), static_cast<usize>(count)});
        total += static_cast<u64>(count);
    }
    if (!file.eof() || total != size || std::filesystem::file_size(path) != size ||
        std::filesystem::last_write_time(path) != time) {
        throw std::runtime_error("native runtime image changed or could not be read completely");
    }
    return hash.hex();
}

#ifdef _WIN32
std::filesystem::path modulePath(HMODULE module) {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("cannot identify the running native module");
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring_view(buffer.data(), length));
        }
        if (buffer.size() >= 32768) {
            throw std::runtime_error("native module path exceeds the supported bound");
        }
        buffer.resize(buffer.size() * 2);
    }
}

#if defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
void verifyStaticRuntime() {
    const HMODULE executable = GetModuleHandleW(nullptr);
    if (executable == nullptr) {
        throw std::runtime_error("cannot identify the static native runtime executable");
    }
    const auto check = [executable](auto function, std::string_view name) {
        HMODULE owner = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(function), &owner) == 0 ||
            owner != executable) {
            throw std::runtime_error("static native runtime function is outside the executable: " +
                                     std::string(name));
        }
    };
    // Check the actual linked function addresses, not the presence of a DLL
    // with a familiar filename. Drivers may legitimately load a second UCRT.
    // The build/import audit covers other translation units; these references
    // independently verify the math and control used by the numeric witness.
    check(&::sinf, "sinf");
    check(&::cosf, "cosf");
    check(&::tanf, "tanf");
    check(&::asinf, "asinf");
    check(&::acosf, "acosf");
    check(&::atan2f, "atan2f");
    check(&::sqrtf, "sqrtf");
    check(&::hypotf, "hypotf");
    check(&::powf, "powf");
    check(&::fmodf, "fmodf");
    check(&::expf, "expf");
    check(&::logf, "logf");
    using Unary = f64 (*)(f64);
    using Binary = f64 (*)(f64, f64);
    check(static_cast<Unary>(&::sin), "sin");
    check(static_cast<Unary>(&::cos), "cos");
    check(static_cast<Unary>(&::tan), "tan");
    check(static_cast<Unary>(&::asin), "asin");
    check(static_cast<Unary>(&::acos), "acos");
    check(static_cast<Binary>(&::atan2), "atan2");
    check(static_cast<Unary>(&::sqrt), "sqrt");
    check(static_cast<Binary>(&::hypot), "hypot");
    check(static_cast<Binary>(&::pow), "pow");
    check(static_cast<Binary>(&::fmod), "fmod");
    check(static_cast<Unary>(&::exp), "exp");
    check(static_cast<Unary>(&::log), "log");
    check(&::_controlfp_s, "_controlfp_s");
#if defined(_M_X64)
    check(&::_get_FMA3_enable, "_get_FMA3_enable");
    check(&::_set_FMA3_enable, "_set_FMA3_enable");
#endif
}
#endif

void runtimeFiles(Digest& digest) {
    digest.text("executable");
    digest.text(fileDigest(modulePath(nullptr)));
#if defined(_MSC_VER) && defined(_MT) && !defined(_DLL)
    // /MT places our runtime in the already-hashed executable. Hashing a UCRT
    // loaded by unrelated graphics/audio code would instead gate on the OS.
    verifyStaticRuntime();
#else
    struct Runtime {
        const wchar_t* native;
        const char* name;
    };
    // Known C/C++ runtime families only: no process-wide renderer/audio/plugin
    // enumeration, and never load a DLL just to manufacture an identity.
    constexpr Runtime runtimes[] = {
        {L"ucrtbase.dll", "ucrtbase"},
        {L"ucrtbased.dll", "ucrtbased"},
        {L"vcruntime140.dll", "vcruntime140"},
        {L"vcruntime140d.dll", "vcruntime140d"},
        {L"vcruntime140_1.dll", "vcruntime140_1"},
        {L"vcruntime140_1d.dll", "vcruntime140_1d"},
        {L"msvcp140.dll", "msvcp140"},
        {L"msvcp140d.dll", "msvcp140d"},
        {L"msvcp140_1.dll", "msvcp140_1"},
        {L"msvcp140_1d.dll", "msvcp140_1d"},
        {L"msvcp140_2.dll", "msvcp140_2"},
        {L"msvcp140_2d.dll", "msvcp140_2d"},
        {L"msvcp140_atomic_wait.dll", "msvcp140_atomic_wait"},
        {L"msvcp140d_atomic_wait.dll", "msvcp140d_atomic_wait"},
        {L"msvcp140_codecvt_ids.dll", "msvcp140_codecvt_ids"},
        {L"msvcp140d_codecvt_ids.dll", "msvcp140d_codecvt_ids"},
    };
    bool ucrt = false;
    bool compilerRuntime = false;
    bool cppRuntime = false;
    for (const auto& runtime : runtimes) {
        digest.text(runtime.name);
        HMODULE handle = nullptr;
        if (GetModuleHandleExW(0, runtime.native, &handle) == 0) {
            digest.word(0);
            continue;
        }
        struct Release {
            HMODULE handle;
            ~Release() { FreeLibrary(handle); }
        } release{handle};
        digest.word(1);
        digest.text(fileDigest(modulePath(handle)));
        const std::string_view name = runtime.name;
        ucrt = ucrt || name == "ucrtbase" || name == "ucrtbased";
        compilerRuntime = compilerRuntime || name == "vcruntime140" || name == "vcruntime140d";
        cppRuntime = cppRuntime || name == "msvcp140" || name == "msvcp140d";
    }
#if defined(_MSC_VER) && defined(_DLL)
    if (!ucrt || !compilerRuntime || !cppRuntime) {
        throw std::runtime_error("a required loaded MSVC runtime could not be identified");
    }
#else
    (void)ucrt;
    (void)compilerRuntime;
    (void)cppRuntime;
#endif
#endif
}
#elif defined(__linux__)
void runtimeFiles(Digest& digest) {
    const auto executable = std::filesystem::read_symlink("/proc/self/exe");
    if (!executable.is_absolute()) {
        throw std::runtime_error("cannot identify the running native executable");
    }
    digest.text("executable");
    // The proc link still identifies the running inode after a file replacement.
    digest.text(fileDigest("/proc/self/exe"));
    constexpr std::string_view symbols[] = {
        "sinf", "cosf", "tanf",   "asinf",  "acosf",  "atan2f", "sqrtf",       "powf", "expf",
        "logf", "sin",  "cos",    "tan",    "asin",   "acos",   "atan2",       "sqrt", "pow",
        "exp",  "log",  "strtof", "strtod", "malloc", "memcpy", "__cxa_throw",
    };
    std::map<std::filesystem::path, std::string> hashes;
    for (const auto symbol : symbols) {
        void* address = dlsym(RTLD_DEFAULT, symbol.data());
        Dl_info information{};
        if (address == nullptr || dladdr(address, &information) == 0 ||
            information.dli_fname == nullptr) {
            throw std::runtime_error("a required loaded native math/runtime symbol is unavailable");
        }
        const auto path = std::filesystem::canonical(information.dli_fname);
        auto found = hashes.find(path);
        if (found == hashes.end()) {
            found = hashes.emplace(path, fileDigest(path)).first;
        }
        digest.text(symbol);
        digest.text(found->second);
    }
}
#else
void runtimeFiles(Digest&) {
    throw std::runtime_error("native online runtime files are unavailable on this OS");
}
#endif

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
#if defined(_WIN32) && defined(_M_X64)
        // UCRT enables FMA3 transcendental implementations at startup according
        // to CPU support; their rounding can differ. Establish one path before
        // any worker starts. Never reset caller-owned modes or silently repair
        // a changed FMA policy during a later room entry.
        windowsControl();
        static std::once_flag prepared;
        std::call_once(prepared, [] {
            if (_set_FMA3_enable(0) != 0) {
                throw std::runtime_error("cannot disable native runtime FMA3 math");
            }
        });
        if (_get_FMA3_enable() != 0) {
            throw std::runtime_error("native runtime FMA3 mode changed after startup");
        }
#endif
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
        return Inputs{strings.digest(), strings.language(), numeric()};
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
    try {
        Digest digest;
        digest.text("gdl-native-runtime-identity-v2");
        platform(digest);
        digest.text(runtimeProfile());
        digest.text(inputs.numeric);
        runtimeFiles(digest);
        return Result{digest.finish(), std::move(inputs.data), std::move(inputs.language)};
    } catch (const std::exception& exception) {
        fail(error, exception.what());
        return std::nullopt;
    }
}

bool NativeOnlineIdentity::numericMatches(const Inputs& inputs) {
    if (!digestText(inputs.numeric)) {
        return false;
    }
    try {
        return inputs.numeric == numeric();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace gdl::game

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "game/netplay/CanonicalDigestSink.h"

namespace {
using namespace gdl;
using namespace gdl::game;

std::vector<u8> digestBytes(usize count) {
    std::vector<u8> bytes(count);
    for (usize index = 0; index < count; ++index) {
        bytes[index] = static_cast<u8>(index * 37 + 11);
    }
    return bytes;
}

std::string hexDigest(const std::array<u8, 32>& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

struct DigestVector {
    usize bytes;
    std::string_view digest;
};
// SHA-256 of byte[i] = (37*i + 11) mod 256, independently generated with
// System.Security.Cryptography.SHA256.HashData, not the implementation under test.
constexpr std::array kVectors{
    DigestVector{0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
    DigestVector{1, "e7cf46a078fed4fafd0b5e3aff144802b853f8ae459a4f0c14add3314b7cc3a6"},
    DigestVector{55, "2900465fcb533e05a158fd2b3be0e5e3b03740d83060aa3580e0d98a96bf2384"},
    DigestVector{56, "31454ff48ef36af2f08fd511bdc37d9d5855ac23e992e5ff5445cb6b7674a674"},
    DigestVector{63, "5f6401b96532c36de4e65beec0409b69b1d181864c8009b7a04f43e5d56350d1"},
    DigestVector{64, "94eb5de4943613fd048dc93393ab06877405faa39c11f53e9386083339833e7e"},
    DigestVector{65, "fc518669b6eb4b4dd91827ecacef86689c725bd5bab888fd3b26dbb196eec954"},
    DigestVector{127, "0fe729ff19257bd6fec853acc2ea355f6b34b58e6c0f684c3e188fcdfcd9baae"},
    DigestVector{128, "0aedd4856f8eba0963627336ad5144a9a7dbe12498e6066f0165fc97d8ddee4c"},
    DigestVector{4095, "14247e71abeac04b1bb56818238f449fe1339e1d9b347ade9224b12460f39d1c"},
    DigestVector{4096, "4e441a3533bb2c10cd5649981d395744213e09a336746b5a3458fee4057205ec"},
    DigestVector{4097, "042a02b55ba342cdd30322331816b6e4bca073f33bdb80149dc74a6f949802f4"},
    DigestVector{16421, "5a6deacc4810fa64a58c0c85400fd0b34fd8d729350ff48b031e863e1f4ce909"},
};

TEST_CASE("canonical digest byte and bulk paths preserve independent boundary vectors",
          "[netplay][canonical-digest][sha256]") {
    for (const auto& expected : kVectors) {
        CAPTURE(expected.bytes);
        const auto bytes = digestBytes(expected.bytes);
        CanonicalDigestSink individual, bulk;
        individual.reserve(bytes.size());
        CHECK(individual.size() == 0);
        for (const auto byte : bytes) {
            individual.push_back(byte);
        }
        bulk.append(bytes);
        CHECK(individual.size() == bytes.size());
        CHECK(bulk.size() == bytes.size());
        CHECK(hexDigest(individual.finish()) == expected.digest);
        CHECK(hexDigest(bulk.finish()) == expected.digest);
        CHECK(individual.finish() == bulk.finish());
        CHECK(individual.finish() == individual.finish());

        Sha256 native;
        native.update(bytes);
        CHECK(native.hex() == expected.digest);
        CHECK(native.finish() == bulk.finish());
    }
}

TEST_CASE("canonical digest mixed append and unaligned SHA blocks preserve all bytes",
          "[netplay][canonical-digest][sha256]") {
    const usize offset = GENERATE(usize{0}, usize{1}, usize{3}, usize{7});
    const usize chunk = GENERATE(usize{1}, usize{17}, usize{63}, usize{64}, usize{65}, usize{4095},
                                 usize{4096}, usize{4097}, usize{8193});
    const auto input = digestBytes(16421);
    std::vector<u8> storage(input.size() + offset + 11, 0xcc);
    std::copy(input.begin(), input.end(), storage.begin() + offset);
    const auto bytes = std::span<const u8>{storage}.subspan(offset, input.size());
    CanonicalDigestSink sink;
    Sha256 hash;
    usize at = 0;
    while (at < bytes.size()) {
        sink.append({});
        hash.update({});
        const usize count = std::min(chunk, bytes.size() - at);
        sink.append(bytes.subspan(at, count));
        hash.update(bytes.subspan(at, count));
        at += count;
        if (at < bytes.size()) {
            sink.push_back(bytes[at]);
            hash.update(bytes.subspan(at, 1));
            ++at;
        }
        CHECK(sink.size() == at);
    }
    CHECK(hexDigest(sink.finish()) == kVectors.back().digest);
    CHECK(hash.hex() == kVectors.back().digest);
    CHECK(sink.size() == bytes.size());
    sink.append({});
    hash.update({});
    CHECK(sink.finish() == hash.finish());
}

TEST_CASE("canonical digest copies and finishing retain independent continuation state",
          "[netplay][canonical-digest][sha256]") {
    const usize split = GENERATE(usize{0}, usize{1}, usize{63}, usize{64}, usize{65}, usize{4095},
                                 usize{4096}, usize{4097}, usize{8192});
    const auto input = digestBytes(16421);
    const auto bytes = std::span<const u8>{input};
    CanonicalDigestSink sink;
    Sha256 hash;
    sink.append(bytes.first(split));
    hash.update(bytes.first(split));
    const auto partial = sink.finish();
    CHECK(partial == hash.finish());
    auto copied = sink;
    auto copiedHash = hash;
    CanonicalDigestSink assigned;
    Sha256 assignedHash;
    assigned.push_back(0xff);
    assignedHash.update(std::array<u8, 1>{0xff});
    assigned = sink;
    assignedHash = hash;

    sink.append(bytes.subspan(split));
    hash.update(bytes.subspan(split));
    CHECK(hexDigest(sink.finish()) == kVectors.back().digest);
    CHECK(sink.finish() == hash.finish());
    CHECK(copied.finish() == partial);
    CHECK(assigned.finish() == partial);
    for (const auto byte : bytes.subspan(split)) {
        copied.push_back(byte);
        copiedHash.update(std::span<const u8>{&byte, 1});
    }
    CHECK(copied.finish() == sink.finish());
    CHECK(copiedHash.finish() == hash.finish());
    CHECK(copied.size() == bytes.size());

    const std::array<u8, 3> suffix{0x80, 0, 0xff};
    assigned.append(suffix);
    assignedHash.update(suffix);
    CHECK(assigned.finish() == assignedHash.finish());
    CHECK(assigned.size() == split + suffix.size());
    CHECK(assigned.finish() != sink.finish());
    CHECK(hexDigest(sink.finish()) == kVectors.back().digest);
}

TEST_CASE("canonical scalar writes retain the bytewise stream across partial buffer boundaries",
          "[netplay][canonical-digest][sha256]") {
    const auto prefix = digestBytes(4097);
    for (usize count : {usize{0}, usize{1}, usize{3}, usize{55}, usize{56}, usize{63},
                        usize{64}, usize{4087}, usize{4088}, usize{4089}, usize{4090},
                        usize{4091}, usize{4092}, usize{4093}, usize{4094}, usize{4095},
                        usize{4096}, usize{4097}}) {
        CAPTURE(count);
        CanonicalDigestSink fast, bytewise;
        fast.append(std::span<const u8>{prefix}.first(count));
        bytewise.append(std::span<const u8>{prefix}.first(count));
        const auto oldWord = [&](u32 value) {
            for (u32 shift = 0; shift < 32; shift += 8) {
                bytewise.push_back(static_cast<u8>(value >> shift));
            }
        };
        for (u32 value : {0U, 0x12345678U, 0x80000000U, 0xffffffffU}) {
            fast.word(value);
            oldWord(value);
            fast.push_back(0x91);
            bytewise.push_back(0x91);
            const u64 wide = (static_cast<u64>(~value) << 32) | value;
            fast.wide(wide);
            oldWord(static_cast<u32>(wide));
            oldWord(static_cast<u32>(wide >> 32));
            CHECK(fast.size() == bytewise.size());
            CHECK(fast.finish() == bytewise.finish());
        }
        // Copies read only the initialized prefix, including a partially full
        // buffer left by a scalar straddling its boundary.
        auto copied = fast;
        CanonicalDigestSink assigned;
        assigned.append(prefix);
        assigned = fast;
        fast.word(42);
        oldWord(42);
        copied.word(42);
        assigned.word(42);
        CHECK(fast.finish() == bytewise.finish());
        CHECK(copied.finish() == bytewise.finish());
        CHECK(assigned.finish() == bytewise.finish());
    }
}
} // namespace

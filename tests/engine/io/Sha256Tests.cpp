#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "engine/io/Sha256.h"

using namespace gdl;

TEST_CASE("SHA256 streaming matches published short and multiblock vectors", "[sha256][netplay]") {
    const auto backend = GENERATE(Sha256::Backend::Automatic, Sha256::Backend::Portable);
    Sha256 hash(backend);
    CHECK(hash.hex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    hash.update(std::array<u8, 3>{'a', 'b', 'c'});
    CHECK(hash.hex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    hash = Sha256{backend};
    const std::string multi = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    for (auto c : multi) {
        hash.update(std::array<u8, 1>{static_cast<u8>(c)});
    }
    CHECK(hash.hex() == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    hash = Sha256{backend};
    const std::vector<u8> chunk(1000, 'a');
    for (usize i = 0; i < 1000; ++i) {
        hash.update(chunk);
    }
    CHECK(hash.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA256 preserves padding boundaries and unaligned streaming", "[sha256][netplay]") {
    const auto backend = GENERATE(Sha256::Backend::Automatic, Sha256::Backend::Portable);
    // Independent SHA256 vectors for byte[i] = i % 251. Exercise both sides of
    // the length-padding boundary, compression blocks and canonical sink chunks.
    const std::array<std::pair<usize, std::string_view>, 11> vectors{{
        {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
        {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
        {63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488"},
        {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
        {65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781"},
        {127, "92ca0fa6651ee2f97b884b7246a562fa71250fedefe5ebf270d31c546bfea976"},
        {128, "471fb943aa23c511f6f72f8d1652d9c880cfa392ad80503120547703e56a2be5"},
        {129, "5099c6a56203f9687f7d33f4bfdf576d31dc91f6b695ecea38b2770c87631135"},
        {4095, "45de2924756389e3ccab98bdaacbef8a81cdeb651b59f916a6d6385b4f7b999d"},
        {4096, "d67c656e01756650d77717b0839985a056ec28ffe174601d690fc407a2ceffca"},
        {4097, "a16560d668b843fb3be99ace41dbd18471f342bd3255a1d21204b35e43f74436"},
    }};
    for (const auto& [size, expected] : vectors) {
        std::vector<u8> storage(size + 1);
        for (usize i = 0; i < size; ++i) {
            storage[i + 1] = static_cast<u8>(i % 251);
        }
        const auto bytes = std::span<const u8>{storage}.subspan(1);
        for (const usize chunk : {usize{1}, usize{7}, usize{55}, usize{64}, usize{65}, size}) {
            CAPTURE(size, chunk);
            Sha256 hash(backend);
            for (usize offset = 0; offset < bytes.size(); offset += chunk) {
                hash.update(bytes.subspan(offset, std::min(chunk, bytes.size() - offset)));
                hash.update({});
            }
            CHECK(hash.hex() == expected);
            CHECK(hash.hex() == expected); // finish is non-destructive
            auto fork = hash;
            hash.update(std::array<u8, 1>{1});
            CHECK(fork.hex() == expected);
            CHECK(hash.hex() != expected);
        }
    }
}

TEST_CASE("SHA256 automatic and portable backends agree for fragmented blocks", "[sha256][netplay]") {
    CAPTURE(Sha256::hardwareAccelerationAvailable());
    std::vector<u8> bytes(16'385);
    u32 random = 0xabc19823;
    for (auto& byte : bytes) {
        random = random * 1664525U + 1013904223U;
        byte = static_cast<u8>(random >> 24);
    }
    for (const usize size : {usize{0}, usize{31}, usize{257}, usize{4093}, bytes.size()}) {
        Sha256 automatic, portable(Sha256::Backend::Portable);
        portable.update(std::span<const u8>{bytes}.first(size));
        for (usize offset = 0; offset < size;) {
            const usize length = std::min(size - offset, usize{1} + bytes[offset]);
            automatic.update(std::span<const u8>{bytes}.subspan(offset, length));
            offset += length;
        }
        CHECK(automatic.finish() == portable.finish());
        automatic.update(bytes);
        portable.update(bytes);
        CHECK(automatic.finish() == portable.finish());
    }
}

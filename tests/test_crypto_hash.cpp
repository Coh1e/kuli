#include <doctest/doctest.h>

#include "kuli/crypto/hash.hpp"

using namespace kuli::crypto;

TEST_CASE("crypto_hash sha256 known answers") {
    // NIST/RFC well-known vectors.
    CHECK(sha256_hex(std::string_view("")) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string_view("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(std::string_view(
              "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("crypto_hash md5 known answer") {
    CHECK(md5_hex(std::string_view("abc")) == "900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE("crypto_hash parse spec") {
    auto a = parse("sha256:ABCDEF");
    REQUIRE(a.has_value());
    CHECK(a->algo == Algorithm::Sha256);
    CHECK(a->hex == "abcdef");  // lowercased

    auto bare = parse("  deadBEEF  ");
    REQUIRE(bare.has_value());
    CHECK(bare->algo == Algorithm::Sha256);
    CHECK(bare->hex == "deadbeef");

    auto md5 = parse("md5:5d41402abc4b2a76b9719d911017c592");
    REQUIRE(md5.has_value());
    CHECK(md5->algo == Algorithm::Md5);

    CHECK_FALSE(parse("nothex:zzzz").has_value());
    CHECK_FALSE(parse("xyz123").has_value());  // non-hex bare string
    CHECK_FALSE(parse("").has_value());
}

TEST_CASE("crypto_hash to_string round-trips") {
    HashSpec s{Algorithm::Sha256, "abc123"};
    CHECK(to_string(s) == "sha256:abc123");
    auto back = parse(to_string(s));
    REQUIRE(back.has_value());
    CHECK(*back == s);
}

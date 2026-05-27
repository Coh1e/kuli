#include <doctest/doctest.h>

#include <string>

#include "kuli/crypto/sign.hpp"

using namespace kuli;

TEST_CASE("sign ed25519 generate / sign / verify roundtrip") {
    auto kp = crypto::ed25519_generate();
    REQUIRE(kp.has_value());
    CHECK(kp->private_pem.find("PRIVATE KEY") != std::string::npos);
    CHECK(kp->public_pem.find("PUBLIC KEY") != std::string::npos);

    auto sig = crypto::ed25519_sign(kp->private_pem, "hello kuli");
    REQUIRE(sig.has_value());
    CHECK(crypto::ed25519_verify(kp->public_pem, "hello kuli", *sig));
}

TEST_CASE("sign ed25519 rejects a tampered message and the wrong key") {
    auto kp = crypto::ed25519_generate();
    REQUIRE(kp.has_value());
    auto sig = crypto::ed25519_sign(kp->private_pem, "payload");
    REQUIRE(sig.has_value());

    CHECK_FALSE(crypto::ed25519_verify(kp->public_pem, "payload-TAMPERED", *sig));
    CHECK_FALSE(crypto::ed25519_verify(kp->public_pem, "payload", "00"));  // bad sig

    auto other = crypto::ed25519_generate();
    REQUIRE(other.has_value());
    CHECK_FALSE(crypto::ed25519_verify(other->public_pem, "payload", *sig));  // wrong key
}

TEST_CASE("sign ed25519 public_of derives the keypair's public key") {
    auto kp = crypto::ed25519_generate();
    REQUIRE(kp.has_value());
    auto pub = crypto::ed25519_public_of(kp->private_pem);
    REQUIRE(pub.has_value());
    CHECK(*pub == kp->public_pem);
}

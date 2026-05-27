#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "kuli/bp/capability.hpp"
#include "kuli/crypto/sign.hpp"

using namespace kuli;

TEST_CASE("capability local record has the expected fields") {
    auto rec = bp::local_capability();
    CHECK_FALSE(rec.value("os", std::string()).empty());
    CHECK_FALSE(rec.value("arch", std::string()).empty());
    CHECK(rec.contains("cpu"));
    CHECK_FALSE(rec.value("kuli", std::string()).empty());
    CHECK(rec["scriptures"].is_array());
}

TEST_CASE("capability matcher handles string / numeric / scripture constraints") {
    nlohmann::json rec = {{"os", "linux"},   {"arch", "x64"},
                          {"host", "build1"}, {"cpu", 16},
                          {"kuli", "0.1.0"},  {"scriptures", {"find", "grep"}}};

    CHECK(bp::capability_matches(rec, {}));  // no constraints -> match
    CHECK(bp::capability_matches(rec, {"os=linux"}));
    CHECK_FALSE(bp::capability_matches(rec, {"os=windows"}));
    CHECK(bp::capability_matches(rec, {"arch=x64", "cpu>=8"}));   // AND
    CHECK_FALSE(bp::capability_matches(rec, {"arch=x64", "cpu>=32"}));
    CHECK(bp::capability_matches(rec, {"cpu>8", "cpu<=16"}));
    CHECK(bp::capability_matches(rec, {"scripture=find"}));
    CHECK_FALSE(bp::capability_matches(rec, {"scripture=docker"}));
    CHECK_FALSE(bp::capability_matches(rec, {"bogus=1"}));  // unknown key
    CHECK_FALSE(bp::capability_matches(rec, {"garbage"}));  // unparseable
}

TEST_CASE("capability verify accepts a signed record and rejects tampering") {
    auto kp = crypto::ed25519_generate();
    REQUIRE(kp.has_value());

    nlohmann::json rec = {{"host", "h"},   {"os", "linux"},
                          {"arch", "x64"}, {"cpu", 8},
                          {"kuli", "0.1.0"}, {"scriptures", nlohmann::json::array()},
                          {"v", 1},        {"pubkey", kp->public_pem}};
    // Sign over the record minus `sig` (matches capability's signing_bytes).
    nlohmann::json to_sign = rec;
    to_sign.erase("sig");
    rec["sig"] = *crypto::ed25519_sign(kp->private_pem, to_sign.dump());

    CHECK(bp::capability_verify(rec));

    nlohmann::json tampered = rec;
    tampered["arch"] = "arm64";
    CHECK_FALSE(bp::capability_verify(tampered));

    nlohmann::json unsigned_rec = rec;
    unsigned_rec.erase("sig");
    CHECK_FALSE(bp::capability_verify(unsigned_rec));
}

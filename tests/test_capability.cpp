#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "kuli/bp/capability.hpp"

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

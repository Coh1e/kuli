#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "kuli/bp/meshell.hpp"

using namespace kuli;

TEST_CASE("meshell a bare command parses to a local Exec") {
    auto ir = bp::parse_meshell("ls -la");
    REQUIRE(ir.has_value());
    CHECK((*ir)["kind"] == "Exec");
    CHECK((*ir)["node"]["at"] == "local:");
    CHECK((*ir)["node"]["cmd"][0] == "ls");
    CHECK((*ir)["node"]["cmd"][1] == "-la");
}

TEST_CASE("meshell @host cmd keeps the alias in at:") {
    auto ir = bp::parse_meshell("@prod ps -ef");
    REQUIRE(ir.has_value());
    CHECK((*ir)["kind"] == "Exec");
    CHECK((*ir)["node"]["at"] == "@prod");
    CHECK((*ir)["node"]["cmd"][0] == "ps");
    CHECK((*ir)["node"]["cmd"][1] == "-ef");
}

TEST_CASE("meshell @local resolves to local:") {
    auto ir = bp::parse_meshell("@local uname -a");
    REQUIRE(ir.has_value());
    CHECK((*ir)["node"]["at"] == "local:");
}

TEST_CASE("meshell colon form @a:cmd splits target and command") {
    auto ir = bp::parse_meshell("@a:cat /file");
    REQUIRE(ir.has_value());
    CHECK((*ir)["node"]["at"] == "@a");
    CHECK((*ir)["node"]["cmd"][0] == "cat");
    CHECK((*ir)["node"]["cmd"][1] == "/file");
}

TEST_CASE("meshell a pipeline parses to Pipeline with per-stage at") {
    auto ir = bp::parse_meshell("@a:cat /f | @b:sort | @local:tee out");
    REQUIRE(ir.has_value());
    CHECK((*ir)["kind"] == "Pipeline");
    const auto& st = (*ir)["node"]["stages"];
    REQUIRE(st.size() == 3);
    CHECK(st[0]["at"] == "@a");
    CHECK(st[0]["cmd"][0] == "cat");
    CHECK(st[1]["at"] == "@b");
    CHECK(st[1]["cmd"][0] == "sort");
    CHECK(st[2]["at"] == "local:");
    CHECK(st[2]["cmd"][0] == "tee");
}

TEST_CASE("meshell rejects empty input and a target without a command") {
    CHECK_FALSE(bp::parse_meshell("").has_value());
    CHECK_FALSE(bp::parse_meshell("   ").has_value());
    CHECK_FALSE(bp::parse_meshell("@prod").has_value());    // target, no command
    CHECK_FALSE(bp::parse_meshell("ls | @b").has_value());  // empty second stage command
}

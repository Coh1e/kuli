#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"
#include "kuli/ir/ir.hpp"

using nlohmann::json;
using kuli::diag::Kind;

namespace {
json valid_apply() {
    return json{
        {"schema", "kuli/ir/1.0"},
        {"kind", "ApplyDerivation"},
        {"options", {{"dry_run", true}}},
        {"node",
         {{"root", "h1"},
          {"derivations",
           json::array({json{{"hash", "h1"},
                             {"name", "x"},
                             {"storePath", "h1-x"},
                             {"builder", "fetch"},
                             {"fetch",
                              {{"owner", "o"},
                               {"repo", "r"},
                               {"version", "1"},
                               {"assetPattern", "p"},
                               {"sha256", std::string(64, 'a')}}}}})}}},
        {"descent_trace", json::array()},
        {"sources", json::array()},
    };
}
}  // namespace

TEST_CASE("ir_validate accepts a well-formed ApplyDerivation") {
    CHECK(kuli::ir::validate(valid_apply()).has_value());
}

TEST_CASE("ir_validate rejects wrong schema -> exit 2") {
    auto doc = valid_apply();
    doc["schema"] = "wrong/1.0";
    auto r = kuli::ir::validate(doc);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::IrValidation);
    CHECK(kuli::diag::exit_code_of(r.error()) == 2);
}

TEST_CASE("ir_validate rejects unknown builder") {
    auto doc = valid_apply();
    doc["node"]["derivations"][0]["builder"] = "magic";
    CHECK_FALSE(kuli::ir::validate(doc).has_value());
}

TEST_CASE("ir_validate rejects malformed sha256 (M-3)") {
    auto doc = valid_apply();
    doc["node"]["derivations"][0]["fetch"]["sha256"] = "not-hex";
    auto r = kuli::ir::validate(doc);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("sha256") != std::string::npos);
}

TEST_CASE("ir_validate rejects fetch missing required field") {
    auto doc = valid_apply();
    doc["node"]["derivations"][0]["fetch"].erase("sha256");
    auto r = kuli::ir::validate(doc);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().message.find("sha256") != std::string::npos);
}

TEST_CASE("ir_validate rejects dangling component reference") {
    auto doc = valid_apply();
    doc["node"]["derivations"][0]["builder"] = "composite";
    doc["node"]["derivations"][0].erase("fetch");
    doc["node"]["derivations"][0]["components"] = json::array({"does-not-exist"});
    CHECK_FALSE(kuli::ir::validate(doc).has_value());
}

TEST_CASE("ir_validate rejects root not in derivations") {
    auto doc = valid_apply();
    doc["node"]["root"] = "ghost";
    CHECK_FALSE(kuli::ir::validate(doc).has_value());
}

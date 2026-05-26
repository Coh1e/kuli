#include <doctest/doctest.h>

#include "kuli/luau/frontend.hpp"

using namespace kuli::luau;
using kuli::diag::Kind;

namespace {
EvalRequest req_with(std::string src) {
    EvalRequest r;
    r.inline_source = std::move(src);
    r.chunk_name = "test";
    r.system = SystemInfo{"windows", "x64", "11"};
    r.source = SourceCtx{"test-src", "C:/tmp", "2026-01-01"};
    return r;
}
}  // namespace

TEST_CASE("luau_sandbox positive: function(ctx) round-trip via composite") {
    auto r = evaluate(req_with(
        R"(return function(ctx)
              return ctx.lib.composite{ name = "demo-" .. ctx.system.os, components = {} }
           end)"));
    REQUIRE(r.has_value());
    REQUIRE(r->root() != nullptr);
    CHECK(r->root()->name == "demo-windows");
    CHECK(r->root()->builder == Builder::Composite);
}

TEST_CASE("luau_sandbox positive: base/math/string/table available") {
    auto r = evaluate(req_with(
        R"(return function(ctx)
              local t = {}
              table.insert(t, string.upper("ok"))
              return ctx.lib.composite{ name = t[1] .. tostring(math.floor(3.7)), components = {} }
           end)"));
    REQUIRE(r.has_value());
    REQUIRE(r->root() != nullptr);
    CHECK(r->root()->name == "OK3");
}

TEST_CASE("luau_sandbox negative: io is forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(return function(ctx)
              local f = io.open("C:/secret", "r")
              return { kind = "derivation", name = "x" }
           end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
    CHECK(kuli::diag::exit_code_of(r.error()) == 3);
    CHECK(r.error().message.find("io") != std::string::npos);
}

TEST_CASE("luau_sandbox negative: os.execute is forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(return function(ctx) os.execute("calc.exe") return {} end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
    CHECK(kuli::diag::exit_code_of(r.error()) == 3);
}

TEST_CASE("luau_sandbox negative: require is forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(local x = require("evil") return function(ctx) return x end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
}

TEST_CASE("luau_sandbox negative: load / loadstring forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(return function(ctx) return load("return 1")() end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
}

TEST_CASE("luau_sandbox negative: _G is forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(return function(ctx) return _G end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
}

TEST_CASE("luau_sandbox negative: debug is forbidden -> exit 3") {
    auto r = evaluate(req_with(
        R"(return function(ctx) return debug.traceback() end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::Sandbox);
}

TEST_CASE("luau_sandbox rule 1: must return a function") {
    auto r = evaluate(req_with(R"(return { kind = "derivation" })"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::General);  // wrong shape, not a sandbox breach
    CHECK(kuli::diag::exit_code_of(r.error()) == 1);
}

TEST_CASE("luau_sandbox rule 1: function must return a table") {
    auto r = evaluate(req_with(R"(return function(ctx) return 42 end)"));
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().kind == Kind::General);
}

TEST_CASE("luau_sandbox resource: infinite loop hits the time budget -> exit 3") {
    EvalRequest r;
    r.inline_source = "return function(ctx) while true do end end";
    r.chunk_name = "test";
    r.system = SystemInfo{"windows", "x64", "11"};
    r.limits.timeout_ms = 200;  // tight budget so the test is fast
    auto res = evaluate(r);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().kind == Kind::Sandbox);
    CHECK(kuli::diag::exit_code_of(res.error()) == 3);
}

TEST_CASE("luau_sandbox resource: memory bomb hits the heap cap and fails fast") {
    EvalRequest r;
    r.inline_source = R"(return function(ctx)
        local t = {}
        for i = 1, 1000000000 do t[i] = string.rep("x", 256) end
        return ctx.lib.composite{ name = "x", components = {} }
    end)";
    r.chunk_name = "test";
    r.system = SystemInfo{"windows", "x64", "11"};
    r.limits.mem_cap_bytes = 8u << 20;  // 8 MiB ceiling
    r.limits.timeout_ms = 10000;
    auto res = evaluate(r);
    REQUIRE_FALSE(res.has_value());  // raised OOM cleanly, did not crash the process
}

TEST_CASE("luau_sandbox syntax error is reported with a span") {
    auto r = evaluate(req_with(R"(return function(ctx) this is not lua)"));
    REQUIRE_FALSE(r.has_value());
    CHECK_FALSE(r.error().spans.empty());
}

#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/sense/sense.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
kuli::engine::RawResult run(const std::string& kind) {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = kind;
    ir["node"] = nlohmann::json::object();
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "t";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    return engine.execute(call);
}
}  // namespace

TEST_CASE("sense host_facts reports a sane host summary") {
    auto f = sense::host_facts();
    CHECK_FALSE(f.os.empty());
    CHECK_FALSE(f.arch.empty());
    CHECK(f.cpu_count >= 1u);
}

TEST_CASE("sense list_processes is non-empty and well-formed") {
    auto ps = sense::list_processes();
    REQUIRE_FALSE(ps.empty());  // every supported platform lists at least this process
    bool any_named = false, any_pid = false;
    for (const auto& p : ps) {
        if (!p.name.empty()) any_named = true;
        if (p.pid > 0) any_pid = true;
    }
    CHECK(any_pid);
    CHECK(any_named);
}

TEST_CASE("sense HostFacts IR renders os/arch lines") {
    auto rr = run(std::string(kuli::ir::kind::HostFacts));
    CHECK(rr.exit_code == 0);
    REQUIRE(rr.lines.size() >= 2);
    bool has_os = false, has_arch = false;
    for (const auto& l : rr.lines) {
        if (l.rfind("os: ", 0) == 0) has_os = true;
        if (l.rfind("arch: ", 0) == 0) has_arch = true;
    }
    CHECK(has_os);
    CHECK(has_arch);
}

TEST_CASE("sense ProcessQuery IR lists processes as pid<TAB>name") {
    auto rr = run(std::string(kuli::ir::kind::ProcessQuery));
    CHECK(rr.exit_code == 0);
    REQUIRE_FALSE(rr.lines.empty());
    CHECK(rr.lines[0].find('\t') != std::string::npos);
}

TEST_CASE("sense EnvQuery IR reads the environment, filtered by name glob") {
#if defined(_WIN32)
    _putenv_s("KULI_TEST_VAR", "hello");
#else
    setenv("KULI_TEST_VAR", "hello", 1);
#endif
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::EnvQuery);
    ir["node"] = {{"name", nlohmann::json::array({"KULI_TEST*"})}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "kenv";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    auto rr = engine.execute(call);

    CHECK(rr.exit_code == 0);
    REQUIRE(rr.lines.size() == 1);  // only the glob-matching var
    CHECK(rr.lines[0] == "KULI_TEST_VAR=hello");
}

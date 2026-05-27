#include <doctest/doctest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/platform/process.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {
// A portable "exit with code N" command.
std::vector<std::string> exit_cmd(int code) {
#if defined(_WIN32)
    return {"cmd", "/c", "exit", std::to_string(code)};
#else
    return {"sh", "-c", "exit " + std::to_string(code)};
#endif
}

kuli::engine::RawResult run_exec(const std::vector<std::string>& cmd, bool dry = false) {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::Exec);
    ir["node"] = {{"at", "local:"}, {"cmd", cmd}};
    if (dry) ir["options"] = {{"dry_run", true}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "x";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    return engine.execute(call);
}
}  // namespace

TEST_CASE("exec run_process returns the child exit code") {
    auto pr = platform::run_process(exit_cmd(7));
    CHECK(pr.launched);
    CHECK(pr.exit_code == 7);
}

TEST_CASE("exec run_process flags a missing command") {
    auto pr = platform::run_process({"this-command-does-not-exist-kuli-xyz"});
    CHECK(pr.exit_code != 0);  // Win: launched=false/-1; POSIX: launched=true, exec fails -> 127
}

TEST_CASE("exec Exec IR runs a command and propagates the exit code") {
    auto rr = run_exec(exit_cmd(3));
    CHECK(rr.exit_code == 3);
}

TEST_CASE("exec Exec IR --dry-run does not run the command") {
    auto rr = run_exec(exit_cmd(3), /*dry=*/true);
    CHECK(rr.exit_code == 0);
    REQUIRE_FALSE(rr.lines.empty());
    CHECK(rr.lines[0].rfind("dry-run:", 0) == 0);
}

TEST_CASE("exec Exec IR rejects an empty cmd") {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::Exec);
    ir["node"] = {{"at", "local:"}, {"cmd", nlohmann::json::array()}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "x";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    auto rr = engine.execute(call);
    CHECK(rr.exit_code != 0);  // validator rejects an empty argv
}

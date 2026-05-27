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

kuli::engine::RawResult run_exec(const std::vector<std::string>& cmd, bool dry = false,
                                 bool capture = false) {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::Exec);
    ir["node"] = {{"at", "local:"}, {"cmd", cmd}, {"capture", capture}};
    if (dry) ir["options"] = {{"dry_run", true}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "x";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    return engine.execute(call);
}

// A portable "print this word" command.
std::vector<std::string> echo_cmd(const std::string& word) {
#if defined(_WIN32)
    return {"cmd", "/c", "echo", word};
#else
    return {"sh", "-c", "echo " + word};
#endif
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

TEST_CASE("exec run_process captures stdout when capture=true") {
    auto pr = platform::run_process(echo_cmd("hi-there"), {}, /*capture=*/true);
    CHECK(pr.launched);
    CHECK(pr.exit_code == 0);
    CHECK(pr.output.find("hi-there") != std::string::npos);
}

TEST_CASE("exec Exec IR capture=true returns output as lines") {
    auto rr = run_exec(echo_cmd("captured-output"), /*dry=*/false, /*capture=*/true);
    CHECK(rr.exit_code == 0);
    bool found = false;
    for (const auto& l : rr.lines) {
        if (l.find("captured-output") != std::string::npos) found = true;
    }
    CHECK(found);
}

TEST_CASE("exec run_process feeds stdin and captures stdout (sort)") {
    auto pr = platform::run_process({"sort"}, {}, /*capture=*/true, /*input=*/"banana\napple\n");
    CHECK(pr.launched);
    auto a = pr.output.find("apple");
    auto b = pr.output.find("banana");
    CHECK(a != std::string::npos);
    CHECK(b != std::string::npos);
    CHECK(a < b);  // sort put apple before banana
}

TEST_CASE("exec transport rejects an unknown @host alias") {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::FileQuery);
    ir["node"] = {{"at", "@kuli-test-nonexistent-alias-xyz"}, {"roots", nlohmann::json::array({"."})}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "x";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    auto rr = engine.execute(call);
    CHECK(rr.exit_code != 0);
    CHECK(rr.raw_stderr.find("unknown host alias") != std::string::npos);
}

TEST_CASE("exec transport rejects an unimplemented at: scheme") {
    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::FileQuery);
    ir["node"] = {{"at", "webdav:host"}, {"roots", nlohmann::json::array({"."})}};
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "x";
    call.cwd = fs::current_path();
    call.ir_doc = ir;
    auto rr = engine.execute(call);
    CHECK(rr.exit_code != 0);  // webdav transport is not implemented yet
    CHECK(rr.raw_stderr.find("not implemented") != std::string::npos);
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

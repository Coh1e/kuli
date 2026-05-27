#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "kuli/bp/generation.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/luau/hashing.hpp"
#include "kuli/store/store.hpp"

namespace fs = std::filesystem;
using namespace kuli;

namespace {

fs::path scratch() {
    static std::mt19937_64 rng{std::random_device{}()};
    fs::path p = fs::temp_directory_path() / ("kuli-scr-" + std::to_string(rng()));
    fs::create_directories(p);
    return p;
}

luau::DerivationGraph eval_src(const std::string& src) {
    luau::EvalRequest req;
    req.inline_source = src;
    req.chunk_name = "t";
    req.system = luau::SystemInfo{"windows", "x64", "11"};
    return luau::evaluate(req).value().graph;
}

const luau::Derivation* root_of(const luau::DerivationGraph& g) { return g.find(g.root); }

// A blueprint that packages a one-basename scripture whose adapter echoes argv.
const char* kScriptureBp = R"(return function(ctx)
    return ctx.lib.mkScripture{
        name = "demo-tools",
        version = "0.1.0",
        basenames = { greet = "adapters/greet.luau" },
        adapters = {
            ["adapters/greet.luau"] =
                "return function(ctx)\n" ..
                "  local who = ctx.argv[1] or 'world'\n" ..
                "  return { lines = { 'hello, ' .. who } }\n" ..
                "end\n",
        },
    }
end)";

// A plain fetch graph (no basenames) for the rollback-reconcile test.
luau::DerivationGraph fetch_graph() {
    return eval_src(R"(return function(ctx)
        return ctx.lib.fetchGitHubRelease{ name="ninja", owner="ninja-build",
            repo="ninja", version="1.12.1", assetPattern="win",
            sha256="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", bin="ninja.exe" }
    end)");
}

bool shim_exists(const fs::path& bin, const std::string& alias) {
    return fs::exists(bin / (alias + ".cmd")) || fs::exists(bin / alias);
}

}  // namespace

TEST_CASE("scripture mkScripture builds a Scripture derivation with a stable hash") {
    auto g = eval_src(kScriptureBp);
    const luau::Derivation* d = root_of(g);
    REQUIRE(d != nullptr);
    CHECK(d->builder == luau::Builder::Scripture);
    CHECK(d->scripture.version == "0.1.0");
    REQUIRE(d->scripture.basenames.count("greet") == 1);
    CHECK(d->scripture.basenames.at("greet") == "adapters/greet.luau");
    CHECK(d->scripture.files.count("adapters/greet.luau") == 1);
    // store path carries the design's "-scripture" suffix (§9.1).
    CHECK(d->store_path == luau::store_path_for_scripture(d->hash, "demo-tools"));
    // Determinism (R-NF-04): same source -> same hash.
    CHECK(root_of(eval_src(kScriptureBp))->hash == d->hash);
}

TEST_CASE("scripture a basename pointing at a missing adapter is rejected") {
    luau::EvalRequest req;
    req.inline_source = R"(return function(ctx)
        return ctx.lib.mkScripture{ name="bad", basenames = { x = "adapters/x.luau" } }
    end)";
    req.chunk_name = "t";
    req.system = luau::SystemInfo{"windows", "x64", "11"};
    CHECK_FALSE(luau::evaluate(req).has_value());  // adapter content not provided
}

TEST_CASE("scripture realize_inline writes files + is idempotent") {
    fs::path root = scratch();
    store::Store st{root, root / "dl"};

    std::vector<std::pair<std::string, std::string>> files = {
        {"manifest.json", "{}"},
        {"adapters/greet.luau", "return function(ctx) return { lines = {} } end"},
        {"resources/banner.txt", "hi"},
    };
    std::string dir_name = "abc0000000000000-demo-scripture";
    std::string content_id(64, 'c');

    auto r1 = st.realize_inline(dir_name, content_id, files);
    REQUIRE(r1.has_value());
    CHECK_FALSE(r1->was_already_present);
    CHECK(fs::exists(r1->store_dir / "adapters" / "greet.luau"));
    CHECK(fs::exists(r1->store_dir / "resources" / "banner.txt"));
    CHECK(fs::exists(r1->store_dir / ".kuli-store-marker.json"));

    auto r2 = st.realize_inline(dir_name, content_id, files);  // cache hit
    REQUIRE(r2.has_value());
    CHECK(r2->was_already_present);

    fs::remove_all(root);
}

TEST_CASE("scripture realize_inline rejects a path that escapes the store") {
    fs::path root = scratch();
    store::Store st{root, root / "dl"};
    std::vector<std::pair<std::string, std::string>> files = {{"../escape", "x"}};
    CHECK_FALSE(st.realize_inline("h-x-scripture", std::string(64, 'd'), files).has_value());
    fs::remove_all(root);
}

TEST_CASE("scripture install projects a basename shim, rollback removes it") {
    fs::path root = scratch();
    fs::path bin = root / "bin";
    fs::path store_root = root / "store";
    bp::Profile p{root / "profiles", "default"};

    bp::Generation g1 = p.commit(eval_src(kScriptureBp));  // scripture (greet)
    REQUIRE(p.activate(g1.id, bin, store_root));
    CHECK(shim_exists(bin, "greet"));

    bp::Generation g2 = p.commit(fetch_graph());           // no basenames
    REQUIRE(p.activate(g2.id, bin, store_root));
    CHECK_FALSE(shim_exists(bin, "greet"));                // reconciled away
    CHECK(shim_exists(bin, "ninja"));

    REQUIRE(p.activate(g1.id, bin, store_root));           // roll back
    CHECK(shim_exists(bin, "greet"));
    CHECK_FALSE(shim_exists(bin, "ninja"));

    fs::remove_all(root);
}

TEST_CASE("scripture adapter runs in the sandbox and renders lines") {
    fs::path dir = scratch();
    {
        std::ofstream o(dir / "greet.luau", std::ios::binary);
        o << "return function(ctx)\n"
             "  local who = ctx.argv[1] or 'world'\n"
             "  return { lines = { 'hello, ' .. who, 'argc=' .. #ctx.argv } }\n"
             "end\n";
    }
    luau::AdapterRequest req;
    req.adapter_path = dir / "greet.luau";
    req.scripture_root = dir;
    req.argv = {"alice"};
    req.system = luau::SystemInfo{"windows", "x64", "11"};

    auto res = luau::evaluate_adapter(req);
    REQUIRE(res.has_value());
    auto lines = res->value.value("lines", std::vector<std::string>{});
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "hello, alice");
    CHECK(lines[1] == "argc=1");

    fs::remove_all(dir);
}

TEST_CASE("scripture adapter is sandboxed (forbidden API rejected)") {
    fs::path dir = scratch();
    {
        std::ofstream o(dir / "bad.luau", std::ios::binary);
        o << "return function(ctx) os.execute('echo hi') return { lines = {} } end\n";
    }
    luau::AdapterRequest req;
    req.adapter_path = dir / "bad.luau";
    req.scripture_root = dir;
    req.system = luau::SystemInfo{"windows", "x64", "11"};
    CHECK_FALSE(luau::evaluate_adapter(req).has_value());  // os.* is forbidden
    fs::remove_all(dir);
}

TEST_CASE("scripture end-to-end: realize from the graph, then run the adapter from the store") {
    fs::path root = scratch();
    store::Store st{root, root / "dl"};

    auto g = eval_src(kScriptureBp);
    const luau::Derivation* d = root_of(g);
    REQUIRE(d != nullptr);

    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& [rel, content] : d->scripture.files) files.emplace_back(rel, content);
    auto r = st.realize_inline(d->store_path, d->hash, files);
    REQUIRE(r.has_value());

    luau::AdapterRequest req;
    req.adapter_path = r->store_dir / fs::path(d->scripture.basenames.at("greet"));
    req.scripture_root = r->store_dir;
    req.argv = {"bob"};
    req.system = luau::SystemInfo{"windows", "x64", "11"};

    auto res = luau::evaluate_adapter(req);
    REQUIRE(res.has_value());
    auto lines = res->value.value("lines", std::vector<std::string>{});
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "hello, bob");

    fs::remove_all(root);
}

TEST_CASE("scripture adapter can emit a FileQuery IR node") {
    fs::path dir = scratch();
    {
        std::ofstream o(dir / "find.luau", std::ios::binary);
        o << "return function(ctx)\n"
             "  return { kind = 'FileQuery', node = {\n"
             "    at = 'local:', roots = { ctx.argv[1] or '.' },\n"
             "    name = { '*.cpp' }, type = 'f' } }\n"
             "end\n";
    }
    luau::AdapterRequest req;
    req.adapter_path = dir / "find.luau";
    req.scripture_root = dir;
    req.argv = {"/some/root"};
    req.system = luau::SystemInfo{"windows", "x64", "11"};

    auto res = luau::evaluate_adapter(req);
    REQUIRE(res.has_value());
    const auto& v = res->value;
    CHECK(v.value("kind", "") == "FileQuery");
    REQUIRE(v["node"]["roots"].is_array());
    CHECK(v["node"]["roots"][0] == "/some/root");
    CHECK(v["node"]["name"][0] == "*.cpp");
    CHECK(v["node"]["type"] == "f");

    fs::remove_all(dir);
}

TEST_CASE("scripture FileQuery walks the filesystem and matches name globs") {
    fs::path dir = scratch();
    fs::create_directories(dir / "sub");
    { std::ofstream(dir / "a.cpp") << "x"; }
    { std::ofstream(dir / "b.hpp") << "x"; }
    { std::ofstream(dir / "sub" / "c.cpp") << "x"; }

    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::FileQuery);
    ir["node"] = {{"at", "local:"},
                  {"roots", nlohmann::json::array({dir.string()})},
                  {"name", nlohmann::json::array({"*.cpp"})},
                  {"type", "f"}};

    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "find";
    call.cwd = dir;
    call.ir_doc = ir;
    auto rr = engine.execute(call);

    CHECK(rr.exit_code == 0);
    REQUIRE(rr.lines.size() == 2);                       // a.cpp + sub/c.cpp; b.hpp excluded
    CHECK(rr.lines[0].find("a.cpp") != std::string::npos);  // sorted: a before sub/c
    CHECK(rr.lines[1].find("c.cpp") != std::string::npos);

    fs::remove_all(dir);
}

TEST_CASE("scripture TextSearch greps file contents and reports path:line:text") {
    fs::path dir = scratch();
    fs::create_directories(dir / "sub");
    { std::ofstream(dir / "a.txt") << "alpha\nTODO: fix\nbeta\n"; }
    { std::ofstream(dir / "sub" / "b.txt") << "nothing here\ntodo lower\n"; }

    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::TextSearch);
    ir["node"] = {{"at", "local:"},
                  {"roots", nlohmann::json::array({dir.string()})},
                  {"pattern", "TODO"}};

    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "grep";
    call.cwd = dir;
    call.ir_doc = ir;
    auto rr = engine.execute(call);

    CHECK(rr.exit_code == 0);
    REQUIRE(rr.lines.size() == 1);  // case-sensitive: "TODO: fix" only ("todo lower" excluded)
    CHECK(rr.lines[0].find("a.txt") != std::string::npos);
    CHECK(rr.lines[0].find(":2:TODO: fix") != std::string::npos);  // line number + text

    fs::remove_all(dir);
}

TEST_CASE("scripture TextSearch supports regex + ignoreCase") {
    fs::path dir = scratch();
    { std::ofstream(dir / "log.txt") << "ERROR 42\nwarn\nerror 7\n"; }

    nlohmann::json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::TextSearch);
    ir["node"] = {{"at", "local:"},
                  {"roots", nlohmann::json::array({dir.string()})},
                  {"pattern", "err.r [0-9]+"},
                  {"regex", true},
                  {"ignoreCase", true}};

    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "grep";
    call.cwd = dir;
    call.ir_doc = ir;
    auto rr = engine.execute(call);

    CHECK(rr.exit_code == 0);
    REQUIRE(rr.lines.size() == 2);  // "ERROR 42" + "error 7"; "warn" excluded

    fs::remove_all(dir);
}

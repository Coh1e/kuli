// luau_frontend.cpp — the sole TU that includes Luau headers (R-NF-03).
//
// Pipeline: parse (syntax + forbidden-global pre-scan, rule 2) → compile →
// load → run module (expect function, rule 1) → build ctx {system, source,
// lib, pkgs} → call function(ctx) → the returned table's hash is the graph
// root. ctx.lib.* closures build derivation nodes and compute their hashes
// C++-side (determinism, R-NF-04); ctx.pkgs.<name> lazily evaluates a sibling
// blueprint (cycle-checked, per-apply cache).
#include "kuli/luau/frontend.hpp"
#include "kuli/luau/pattern.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <lua.h>
#include <lualib.h>
#include <luacode.h>

#include "Luau/Ast.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"

#include "kuli/luau/hashing.hpp"

namespace kuli::luau {

namespace fs = std::filesystem;

namespace {

using kuli::diag::Diagnostic;
using kuli::diag::Kind;
using kuli::diag::Span;

// ----- per-apply evaluation state (C++, reachable from closures via upvalue)
struct EvalState {
    SystemInfo system;
    SourceCtx source;
    std::string system_target;       // "<os>-<arch>"
    DerivationGraph graph;
    int ctx_ref = LUA_NOREF;         // registry ref to the ctx table
    std::map<std::string, int> pkg_cache;  // name -> registry ref of its derivation table
    std::set<std::string> in_progress;     // cycle detection
    std::optional<Diagnostic> pending;     // rich diagnostic to surface past luaL_error
};

EvalState* state_of(lua_State* L) {
    return static_cast<EvalState*>(lua_tolightuserdata(L, lua_upvalueindex(1)));
}

// ----- §8.2.3 sandbox -------------------------------------------------------
const std::set<std::string>& forbidden_globals() {
    static const std::set<std::string> s = {
        "require", "io", "os", "package", "loadstring", "load", "loadfile",
        "dofile", "getfenv", "setfenv", "_G", "collectgarbage", "debug",
        "newproxy", "coroutine", "bit32", "buffer",
    };
    return s;
}

struct ForbiddenScan : Luau::AstVisitor {
    struct Hit { std::string name; int line; int col; };
    std::vector<Hit> hits;
    bool visit(Luau::AstExprGlobal* g) override {
        std::string name = g->name.value ? g->name.value : "";
        if (forbidden_globals().count(name)) {
            hits.push_back({name, static_cast<int>(g->location.begin.line) + 1,
                            static_cast<int>(g->location.begin.column) + 1});
        }
        return true;
    }
};

std::string read_file(const fs::path& p, bool& ok) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { ok = false; return {}; }
    std::ostringstream ss;
    ss << in.rdbuf();
    ok = true;
    return ss.str();
}

const char* safe_tostring(lua_State* L, int idx) {
    const char* s = lua_tostring(L, idx);
    return s ? s : "(non-string error value)";
}

void push_str(lua_State* L, const std::string& s) {
    lua_pushlstring(L, s.data(), s.size());
}

std::optional<std::string> opt_field(lua_State* L, int tidx, const char* key) {
    lua_getfield(L, tidx, key);
    std::optional<std::string> v;
    if (lua_type(L, -1) == LUA_TSTRING) v = lua_tostring(L, -1);
    lua_pop(L, 1);
    return v;
}

std::string req_field(lua_State* L, int tidx, const char* key, const char* fn) {
    auto v = opt_field(L, tidx, key);
    if (!v) luaL_error(L, "%s: missing required string field '%s'", fn, key);
    return *v;
}

bool is_sha256_hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Build {kind="derivation", hash, name, storePath, builder} and leave it on
// the stack as the closure's single return value.
void push_derivation_table(lua_State* L, const Derivation& d) {
    lua_newtable(L);
    lua_pushstring(L, "derivation");
    lua_setfield(L, -2, "kind");
    push_str(L, d.hash);
    lua_setfield(L, -2, "hash");
    push_str(L, d.name);
    lua_setfield(L, -2, "name");
    push_str(L, d.store_path);
    lua_setfield(L, -2, "storePath");
    lua_pushstring(L, builder_name(d.builder));
    lua_setfield(L, -2, "builder");
}

// Read the "hash" of a derivation table at `idx`; errors if it isn't a
// derivation produced by ctx.lib.*.
std::string child_hash(lua_State* L, int idx, EvalState* st, const char* fn) {
    if (lua_type(L, idx) != LUA_TTABLE) {
        luaL_error(L, "%s: expected a derivation, got %s", fn, luaL_typename(L, idx));
    }
    lua_getfield(L, idx, "hash");
    if (lua_type(L, -1) != LUA_TSTRING) {
        luaL_error(L, "%s: argument is not a derivation (build it with ctx.lib.*)", fn);
    }
    std::string h = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!st->graph.find(h)) {
        luaL_error(L, "%s: unknown derivation hash (not produced in this evaluation)", fn);
    }
    return h;
}

// ----- lib.* closures -------------------------------------------------------
int lib_fetch(lua_State* L) {
    EvalState* st = state_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    Derivation d;
    d.builder = Builder::Fetch;
    d.name = req_field(L, 1, "name", "fetchGitHubRelease");
    d.fetch.owner = req_field(L, 1, "owner", "fetchGitHubRelease");
    d.fetch.repo = req_field(L, 1, "repo", "fetchGitHubRelease");
    d.fetch.version = req_field(L, 1, "version", "fetchGitHubRelease");
    d.fetch.asset_pattern = req_field(L, 1, "assetPattern", "fetchGitHubRelease");
    d.fetch.sha256 = req_field(L, 1, "sha256", "fetchGitHubRelease");
    if (!is_sha256_hex(d.fetch.sha256)) {
        luaL_error(L, "fetchGitHubRelease: sha256 must be 64 hex chars (got %d)",
                   static_cast<int>(d.fetch.sha256.size()));
    }
    d.fetch.sha256 = to_lower(d.fetch.sha256);  // normalize so case can't fork the hash
    d.fetch.bin = opt_field(L, 1, "bin").value_or("");
    d.fetch.shim_dir = opt_field(L, 1, "shimDir").value_or("");
    d.fetch.post_install = opt_field(L, 1, "postInstall").value_or("");
    d.system_target = st->system_target;
    d.hash = hash_fetch(d.fetch, d.name, d.system_target);
    d.store_path = store_path_for(d.hash, d.name);
    st->graph.nodes[d.hash] = d;
    push_derivation_table(L, d);
    return 1;
}

std::vector<std::string> read_child_array(lua_State* L, int tidx, const char* field,
                                          EvalState* st, const char* fn) {
    std::vector<std::string> out;
    lua_getfield(L, tidx, field);
    if (lua_type(L, -1) == LUA_TTABLE) {
        int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            out.push_back(child_hash(L, -1, st, fn));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    return out;
}

int lib_composite(lua_State* L) {
    EvalState* st = state_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    Derivation d;
    d.builder = Builder::Composite;
    d.name = req_field(L, 1, "name", "composite");
    d.components = read_child_array(L, 1, "components", st, "composite");
    d.requires_ = read_child_array(L, 1, "requires", st, "composite");
    d.system_target = st->system_target;
    d.hash = hash_composite(d.name, d.components, d.requires_);
    d.store_path = store_path_for(d.hash, d.name);
    st->graph.nodes[d.hash] = d;
    push_derivation_table(L, d);
    return 1;
}

int lib_withfiles(lua_State* L) {
    EvalState* st = state_of(L);
    std::string base = child_hash(L, 1, st, "withFiles");
    luaL_checktype(L, 2, LUA_TTABLE);

    std::vector<FileEntry> files;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        // key at -2 (path string), value at -1 (table {mode, content})
        if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TTABLE) {
            FileEntry e;
            e.path = lua_tostring(L, -2);
            e.mode = opt_field(L, -1, "mode").value_or("replace");
            e.content = opt_field(L, -1, "content").value_or("");
            files.push_back(std::move(e));
        }
        lua_pop(L, 1);  // pop value, keep key
    }

    const Derivation* basenode = st->graph.find(base);
    Derivation d;
    d.builder = Builder::WithFiles;
    d.name = basenode ? basenode->name : std::string("withFiles");
    d.base = base;
    d.files = files;
    d.system_target = st->system_target;
    d.hash = hash_withfiles(d.name, base, files);
    d.store_path = store_path_for(d.hash, d.name);
    st->graph.nodes[d.hash] = d;
    push_derivation_table(L, d);
    return 1;
}

// Defined below (shared with lib_read_resource); declared here for mkScripture.
bool unsafe_rel(const std::string& rel);

// Read a Luau table of { string = string } into `out` (overwriting). Non-string
// keys/values are skipped. Used for mkScripture's basenames / adapters / resources.
void read_string_map(lua_State* L, int tidx, const char* field,
                     std::map<std::string, std::string>& out) {
    lua_getfield(L, tidx, field);
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {  // key at -2, value at -1
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING) {
                std::string key = lua_tostring(L, -2);
                std::size_t len = 0;
                const char* c = lua_tolstring(L, -1, &len);
                out[key] = std::string(c, len);
            }
            lua_pop(L, 1);  // pop value, keep key
        }
    }
    lua_pop(L, 1);
}

int lib_mkscripture(lua_State* L) {
    EvalState* st = state_of(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    Derivation d;
    d.builder = Builder::Scripture;
    d.name = req_field(L, 1, "name", "mkScripture");
    d.scripture.version = opt_field(L, 1, "version").value_or("0.0.0");

    read_string_map(L, 1, "basenames", d.scripture.basenames);
    if (d.scripture.basenames.empty()) {
        luaL_error(L, "mkScripture: 'basenames' must map at least one basename to an adapter path");
    }
    // adapters + resources share one rel-path -> content map (the store layout).
    read_string_map(L, 1, "adapters", d.scripture.files);
    read_string_map(L, 1, "resources", d.scripture.files);

    for (const auto& [alias, rel] : d.scripture.basenames) {
        if (unsafe_rel(rel)) {
            luaL_error(L, "mkScripture: unsafe adapter path for basename '%s': %s", alias.c_str(),
                       rel.c_str());
        }
        if (!d.scripture.files.count(rel)) {
            luaL_error(L,
                       "mkScripture: basename '%s' maps to adapter '%s' which is not provided in "
                       "adapters/resources",
                       alias.c_str(), rel.c_str());
        }
    }
    for (const auto& [rel, _] : d.scripture.files) {
        if (unsafe_rel(rel)) luaL_error(L, "mkScripture: unsafe file path: %s", rel.c_str());
    }

    d.system_target = st->system_target;
    d.hash = hash_scripture(d.name, d.scripture);
    d.store_path = store_path_for_scripture(d.hash, d.name);
    st->graph.nodes[d.hash] = d;
    push_derivation_table(L, d);
    return 1;
}

int lib_merge(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_newtable(L);
    int res = lua_gettop(L);
    for (int src : {1, 2}) {  // b (src=2) overrides a (src=1)
        lua_pushnil(L);
        while (lua_next(L, src) != 0) {
            lua_pushvalue(L, -2);  // key
            lua_pushvalue(L, -2);  // value
            lua_settable(L, res);
            lua_pop(L, 1);  // pop value, keep key
        }
    }
    return 1;  // result is on top
}

// Reject absolute / drive-qualified paths and any ".." *segment* (a substring
// check would wrongly reject a legitimate name like "my..cfg").
bool unsafe_rel(const std::string& rel) {
    if (rel.empty()) return true;
    if (rel.front() == '/' || rel.front() == '\\') return true;
    if (rel.size() >= 2 && rel[1] == ':') return true;
    std::string seg;
    for (char c : rel) {
        if (c == '/' || c == '\\') {
            if (seg == "..") return true;
            seg.clear();
        } else {
            seg.push_back(c);
        }
    }
    return seg == "..";
}

int lib_read_resource(lua_State* L) {
    EvalState* st = state_of(L);
    const char* rel_c = luaL_checkstring(L, 1);
    std::string rel = rel_c;
    if (unsafe_rel(rel)) {
        luaL_error(L, "readResource: unsafe path (absolute or '..'): %s", rel.c_str());
    }
    fs::path p = st->source.root / "resources" / fs::path(rel);
    bool ok = false;
    std::string data = read_file(p, ok);
    if (!ok) luaL_error(L, "readResource: not found: %s", p.string().c_str());
    push_str(L, data);
    return 1;
}

// ----- pkgs lazy proxy (§8.2.2) ---------------------------------------------
// Forward declaration; eval_blueprint_function is the recursive evaluator.
bool eval_blueprint_function(lua_State* L, EvalState* st, const std::string& source,
                             const std::string& chunk);  // leaves function on stack

int pkg_index(lua_State* L) {
    EvalState* st = state_of(L);
    // args: (pkgs_table, key)
    const char* key_c = luaL_checkstring(L, 2);
    std::string key = key_c;

    auto cached = st->pkg_cache.find(key);
    if (cached != st->pkg_cache.end()) {
        lua_getref(L, cached->second);
        return 1;
    }
    if (st->in_progress.count(key)) {
        luaL_error(L, "cyclic blueprint dependency at '%s'", key.c_str());
    }

    // Resolve the sibling blueprint file: <root>/blueprints/<key>.luau, then
    // <root>/<key>.luau (flat layout).
    fs::path a = st->source.root / "blueprints" / (key + ".luau");
    fs::path b = st->source.root / (key + ".luau");
    fs::path file = fs::exists(a) ? a : b;
    bool ok = false;
    std::string src = read_file(file, ok);
    if (!ok) {
        luaL_error(L, "blueprint not found: '%s' (looked in %s)", key.c_str(),
                   (st->source.root / "blueprints").string().c_str());
    }

    // Forbidden-global pre-scan of the sibling (rule 2). Surface as Sandbox.
    Luau::Allocator alloc;
    Luau::AstNameTable names(alloc);
    Luau::ParseResult pr = Luau::Parser::parse(src.data(), src.size(), names, alloc, {});
    if (!pr.errors.empty()) {
        st->pending = Diagnostic::of(Kind::General,
                                     "syntax error in blueprint '" + key + "': " +
                                         pr.errors.front().getMessage(),
                                     "E0101");
        luaL_error(L, "syntax error in blueprint '%s'", key.c_str());
    }
    ForbiddenScan scan;
    if (pr.root) pr.root->visit(&scan);
    if (!scan.hits.empty()) {
        st->pending = Diagnostic::of(
            Kind::Sandbox,
            "blueprint '" + key + "' uses forbidden API '" + scan.hits.front().name + "'",
            "E0301");
        luaL_error(L, "sandbox violation in blueprint '%s'", key.c_str());
    }

    st->in_progress.insert(key);
    if (!eval_blueprint_function(L, st, src, key)) {  // leaves function on stack
        st->in_progress.erase(key);
        luaL_error(L, "failed to load blueprint '%s'", key.c_str());
    }
    // Call function(ctx) with the shared ctx.
    lua_getref(L, st->ctx_ref);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        std::string msg = safe_tostring(L, -1);
        st->in_progress.erase(key);
        if (!st->pending) st->pending = Diagnostic::of(Kind::General, msg, "E0107");
        luaL_error(L, "error evaluating blueprint '%s'", key.c_str());
    }
    st->in_progress.erase(key);
    if (lua_type(L, -1) != LUA_TTABLE) {
        luaL_error(L, "blueprint '%s' did not return a derivation", key.c_str());
    }
    // Cache a ref to the returned derivation table (lua_ref does not pop).
    st->pkg_cache[key] = lua_ref(L, -1);
    return 1;  // returned derivation table on top
}

// ----- resource limits (H-1) ------------------------------------------------
// Heap ceiling via a custom lua_Alloc, and a wall-clock budget enforced from
// the VM interrupt (fired at loop back-edges / calls). Both bound a hostile or
// buggy blueprint so it can't hang or OOM the process.
struct AllocState {
    std::size_t used = 0;
    std::size_t cap = 0;
};

void* capped_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto* a = static_cast<AllocState*>(ud);
    if (nsize == 0) {
        std::free(ptr);
        if (a && ptr) a->used -= osize;
        return nullptr;
    }
    std::size_t next = a->used - (ptr ? osize : 0) + nsize;
    if (a->cap && next > a->cap) return nullptr;  // deny -> Luau raises OOM
    void* p = std::realloc(ptr, nsize);
    if (p) a->used = next;
    return p;
}

struct Limits {
    std::chrono::steady_clock::time_point deadline;
    bool time_tripped = false;
};

void on_interrupt(lua_State* L, int /*gc*/) {
    auto* lim = static_cast<Limits*>(lua_callbacks(L)->userdata);
    if (!lim) return;
    if (std::chrono::steady_clock::now() > lim->deadline) {
        lim->time_tripped = true;
        lua_callbacks(L)->interrupt = nullptr;  // avoid re-entry while unwinding
        luaL_error(L, "blueprint exceeded its evaluation time budget");
    }
}

// Shared "exceeded the evaluation time budget" diagnostic (H-1 resource limit).
Diagnostic time_budget_diag(const char* what) {
    return Diagnostic::of(Kind::Sandbox,
                          std::string(what) + " exceeded its evaluation time budget "
                          "(R-NF-03 resource limit)",
                          "E0302")
        .with_help("must terminate quickly and purely; remove unbounded loops");
}

// ----- ctx + VM -------------------------------------------------------------
struct Vm {
    AllocState alloc_;
    Limits limits_;
    lua_State* L = nullptr;

    explicit Vm(const EvalLimits& lims) {
        alloc_.cap = lims.mem_cap_bytes;
        L = lua_newstate(&capped_alloc, &alloc_);
        if (!L) return;
        lua_callbacks(L)->userdata = &limits_;
        lua_callbacks(L)->interrupt = &on_interrupt;
        limits_.deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(lims.timeout_ms);
        luaL_openlibs(L);
        strip_dangerous();
        luaL_sandbox(L);
    }
    ~Vm() { if (L) lua_close(L); }
    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    bool time_tripped() const { return limits_.time_tripped; }

    void strip_dangerous() {
        for (const char* g :
             {"os", "debug", "coroutine", "bit32", "buffer", "require", "package",
              "loadstring", "load", "loadfile", "dofile", "getfenv", "setfenv",
              "_G", "collectgarbage", "newproxy"}) {
            lua_pushnil(L);
            lua_setglobal(L, g);
        }
        lua_getglobal(L, "math");
        if (lua_istable(L, -1)) {
            lua_pushnil(L); lua_setfield(L, -2, "random");
            lua_pushnil(L); lua_setfield(L, -2, "randomseed");
        }
        lua_pop(L, 1);
    }
};

void add_closure(lua_State* L, EvalState* st, const char* name, lua_CFunction fn) {
    lua_pushlightuserdata(L, st);
    lua_pushcclosure(L, fn, name, 1);
    lua_setfield(L, -2, name);  // assumes target table just below
}

// Build the ctx table on top of the stack. `argv` populates ctx.argv for
// scripture adapters (empty for blueprints, which ignore it).
void build_ctx(lua_State* L, EvalState* st, const std::vector<std::string>& argv = {}) {
    lua_newtable(L);  // ctx

    lua_newtable(L);  // ctx.system
    push_str(L, st->system.os);          lua_setfield(L, -2, "os");
    push_str(L, st->system.arch);        lua_setfield(L, -2, "arch");
    push_str(L, st->system.win_version); lua_setfield(L, -2, "winVersion");
    lua_setfield(L, -2, "system");

    lua_newtable(L);  // ctx.argv (1-based array)
    for (std::size_t i = 0; i < argv.size(); ++i) {
        push_str(L, argv[i]);
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    lua_setfield(L, -2, "argv");

    lua_newtable(L);  // ctx.source
    push_str(L, st->source.name);            lua_setfield(L, -2, "name");
    push_str(L, st->source.root.string());   lua_setfield(L, -2, "root");
    push_str(L, st->source.locked_at);       lua_setfield(L, -2, "lockedAt");
    lua_setfield(L, -2, "source");

    lua_newtable(L);  // ctx.lib
    add_closure(L, st, "fetchGitHubRelease", lib_fetch);
    add_closure(L, st, "composite", lib_composite);
    add_closure(L, st, "withFiles", lib_withfiles);
    add_closure(L, st, "mkScripture", lib_mkscripture);
    add_closure(L, st, "merge", lib_merge);
    add_closure(L, st, "readResource", lib_read_resource);
    lua_setfield(L, -2, "lib");

    lua_newtable(L);  // ctx.pkgs (proxy)
    lua_newtable(L);  // metatable
    lua_pushlightuserdata(L, st);
    lua_pushcclosure(L, pkg_index, "pkgs.__index", 1);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setfield(L, -2, "pkgs");
}

// Compile + load + run `source`, leaving the returned `function(ctx)` on the
// stack. Returns false (with a message on the stack) on failure.
bool eval_blueprint_function(lua_State* L, EvalState* /*st*/, const std::string& source,
                             const std::string& chunk) {
    Luau::CompileOptions copts;
    copts.optimizationLevel = 1;
    copts.debugLevel = 1;
    std::string bytecode = Luau::compile(source, copts);

    std::string chunkname = "=" + chunk;
    if (luau_load(L, chunkname.c_str(), bytecode.data(), bytecode.size(), 0) != 0) {
        return false;  // error string on stack
    }
    if (lua_pcall(L, 0, 1, 0) != 0) {
        return false;  // error string on stack
    }
    if (lua_type(L, -1) != LUA_TFUNCTION) {
        lua_pop(L, 1);
        lua_pushstring(L, "blueprint must return function(ctx)");
        return false;
    }
    return true;
}

}  // namespace

std::expected<EvalResult, Diagnostic> evaluate(const EvalRequest& req) {
    std::string source = req.inline_source;
    std::string chunk = req.chunk_name;
    if (source.empty()) {
        bool ok = false;
        source = read_file(req.blueprint_path, ok);
        if (!ok) {
            return std::unexpected(Diagnostic::error(
                "cannot read blueprint file: " + req.blueprint_path.string(), "E0100"));
        }
        chunk = req.blueprint_path.filename().string();
    }

    // 1) Parse: syntax errors + forbidden-global pre-scan (rule 2).
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult pr =
        Luau::Parser::parse(source.data(), source.size(), names, allocator, {});
    if (!pr.errors.empty()) {
        const auto& e = pr.errors.front();
        Diagnostic d = Diagnostic::of(Kind::General, e.getMessage(), "E0101");
        d.with_span(Span{chunk, static_cast<int>(e.getLocation().begin.line) + 1,
                         static_cast<int>(e.getLocation().begin.column) + 1, "syntax error"});
        return std::unexpected(std::move(d));
    }
    ForbiddenScan scan;
    if (pr.root) pr.root->visit(&scan);
    if (!scan.hits.empty()) {
        const auto& h = scan.hits.front();
        Diagnostic d = Diagnostic::of(
            Kind::Sandbox,
            "blueprint uses forbidden API '" + h.name + "' (Luau sandbox, R-NF-03)", "E0301");
        d.with_span(Span{chunk, h.line, h.col, "forbidden in sandbox"});
        d.with_help(
            "blueprints are pure: use ctx.lib.readResource to read files and derivation "
            "builders to cause effects");
        return std::unexpected(std::move(d));
    }

    Vm vm(req.limits);
    if (!vm.L) {
        return std::unexpected(Diagnostic::of(Kind::Internal, "failed to create Luau VM", "E7001"));
    }
    lua_State* L = vm.L;

    auto resource_diag = [&]() {
        return Diagnostic::of(Kind::Sandbox,
                              "blueprint exceeded its evaluation time budget (R-NF-03 resource limit)",
                              "E0302")
            .with_help("blueprints must terminate quickly and purely; remove unbounded loops");
    };

    EvalState st;
    st.system = req.system;
    st.source = req.source;
    st.system_target = req.system.os + "-" + req.system.arch;

    if (!eval_blueprint_function(L, &st, source, chunk)) {
        if (vm.time_tripped()) return std::unexpected(resource_diag());
        return std::unexpected(Diagnostic::of(Kind::General, safe_tostring(L, -1), "E0103"));
    }
    // function(ctx) is on the stack. Build ctx and stash a ref for pkgs.
    // Luau's lua_ref pins the value WITHOUT popping it, so ctx stays on the
    // stack as the pcall argument while the registry ref keeps it alive for
    // recursive pkgs resolution.
    build_ctx(L, &st);            // [function, ctx]
    st.ctx_ref = lua_ref(L, -1);  // ref ctx (no pop)
    if (lua_pcall(L, 1, 1, 0) != 0) {
        Diagnostic d = vm.time_tripped() ? resource_diag()
                       : st.pending      ? *st.pending
                                         : Diagnostic::of(Kind::General, safe_tostring(L, -1), "E0106");
        lua_unref(L, st.ctx_ref);
        return std::unexpected(std::move(d));
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_unref(L, st.ctx_ref);
        return std::unexpected(Diagnostic::error(
            "blueprint function must return a Derivation table (rule 1)", "E0104"));
    }

    // The returned table's hash is the graph root.
    lua_getfield(L, -1, "hash");
    std::string root_hash = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    lua_unref(L, st.ctx_ref);

    if (root_hash.empty() || !st.graph.find(root_hash)) {
        return std::unexpected(Diagnostic::error(
            "blueprint must return a derivation built with ctx.lib.* (rule 1)", "E0108"));
    }
    st.graph.root = root_hash;

    EvalResult result;
    result.graph = std::move(st.graph);
    return result;
}

std::expected<AdapterResult, Diagnostic> evaluate_adapter(const AdapterRequest& req) {
    bool ok = false;
    std::string source = read_file(req.adapter_path, ok);
    if (!ok) {
        return std::unexpected(Diagnostic::error(
            "cannot read scripture adapter: " + req.adapter_path.string(), "E0950"));
    }
    std::string chunk = req.adapter_path.filename().string();

    // Same parse + forbidden-global pre-scan as a blueprint (R-NF-03): a
    // third-party adapter is untrusted code.
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    Luau::ParseResult pr =
        Luau::Parser::parse(source.data(), source.size(), names, allocator, {});
    if (!pr.errors.empty()) {
        const auto& e = pr.errors.front();
        Diagnostic d = Diagnostic::of(Kind::General, e.getMessage(), "E0951");
        d.with_span(Span{chunk, static_cast<int>(e.getLocation().begin.line) + 1,
                         static_cast<int>(e.getLocation().begin.column) + 1, "syntax error"});
        return std::unexpected(std::move(d));
    }
    ForbiddenScan scan;
    if (pr.root) pr.root->visit(&scan);
    if (!scan.hits.empty()) {
        const auto& h = scan.hits.front();
        Diagnostic d = Diagnostic::of(
            Kind::Sandbox,
            "scripture adapter uses forbidden API '" + h.name + "' (Luau sandbox, R-NF-03)",
            "E0301");
        d.with_span(Span{chunk, h.line, h.col, "forbidden in sandbox"});
        return std::unexpected(std::move(d));
    }

    Vm vm(req.limits);
    if (!vm.L) {
        return std::unexpected(Diagnostic::of(Kind::Internal, "failed to create Luau VM", "E7001"));
    }
    lua_State* L = vm.L;

    EvalState st;
    st.system = req.system;
    st.source = SourceCtx{"", req.scripture_root, ""};  // readResource → <store>/resources
    st.system_target = req.system.os + "-" + req.system.arch;

    if (!eval_blueprint_function(L, &st, source, chunk)) {
        if (vm.time_tripped()) return std::unexpected(time_budget_diag("scripture adapter"));
        return std::unexpected(Diagnostic::of(Kind::General, safe_tostring(L, -1), "E0952"));
    }
    build_ctx(L, &st, req.argv);   // [function, ctx]
    st.ctx_ref = lua_ref(L, -1);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        Diagnostic d = vm.time_tripped() ? time_budget_diag("scripture adapter")
                       : st.pending      ? *st.pending
                                         : Diagnostic::of(Kind::General, safe_tostring(L, -1), "E0953");
        lua_unref(L, st.ctx_ref);
        return std::unexpected(std::move(d));
    }
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_unref(L, st.ctx_ref);
        return std::unexpected(Diagnostic::error(
            "scripture adapter must return a result table { lines = { ... } }", "E0954"));
    }

    AdapterResult res;
    lua_getfield(L, -1, "lines");
    if (lua_type(L, -1) == LUA_TTABLE) {
        int n = lua_objlen(L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_type(L, -1) == LUA_TSTRING) {
                std::size_t len = 0;
                const char* c = lua_tolstring(L, -1, &len);
                res.lines.emplace_back(c, len);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);  // pop lines
    lua_unref(L, st.ctx_ref);
    return res;
}

}  // namespace kuli::luau

namespace kuli::luau {

bool pattern_matches(std::string_view text, std::string_view pattern) {
    // A throwaway VM with just the string lib; no sandbox needed (no user code
    // runs — we only call string.find on host-controlled inputs).
    lua_State* L = luaL_newstate();
    if (!L) return false;
    luaL_openlibs(L);
    lua_getglobal(L, "string");
    if (!lua_istable(L, -1)) { lua_close(L); return false; }
    lua_getfield(L, -1, "find");
    if (!lua_isfunction(L, -1)) { lua_close(L); return false; }
    lua_pushlstring(L, text.data(), text.size());
    lua_pushlstring(L, pattern.data(), pattern.size());
    bool matched = false;
    if (lua_pcall(L, 2, 1, 0) == 0) {
        matched = lua_isnumber(L, -1);  // start index on match; nil otherwise
    }
    lua_close(L);
    return matched;
}

const char* builder_name(Builder b) {
    switch (b) {
        case Builder::Fetch:     return "fetch";
        case Builder::Composite: return "composite";
        case Builder::WithFiles: return "withFiles";
        case Builder::Scripture: return "scripture";
    }
    return "fetch";
}
}  // namespace kuli::luau

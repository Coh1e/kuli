#include "kuli/bp/scripture.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

#include "kuli/bp/generation.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/luau/hashing.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/platform/shim.hpp"
#include "kuli/platform/win_env.hpp"
#include "kuli/store/store.hpp"

namespace kuli::bp {

namespace {

using nlohmann::json;
namespace paths = kuli::platform::paths;

// ----- built-in scriptures (shipped embedded, §3 share/kuli/scriptures) -----
// `find <root> [-name <glob>]...` -> FileQuery.
constexpr const char* kFindAdapter =
    R"LUAU(return function(ctx)
  local root = ctx.argv[1] or "."
  local names = {}
  local i = 2
  while ctx.argv[i] do
    if ctx.argv[i] == "-name" and ctx.argv[i + 1] then
      table.insert(names, ctx.argv[i + 1]); i = i + 2
    else i = i + 1 end
  end
  return { kind = "FileQuery", node = { at = "local:", roots = { root }, name = names, type = "f" } }
end
)LUAU";

// `grep <pattern> <root> [-i] [-name <glob>]...` -> TextSearch.
constexpr const char* kGrepAdapter =
    R"LUAU(return function(ctx)
  local pattern = ctx.argv[1] or ""
  local root = ctx.argv[2] or "."
  local icase = false
  local names = {}
  local i = 3
  while ctx.argv[i] do
    if ctx.argv[i] == "-i" then icase = true; i = i + 1
    elseif ctx.argv[i] == "-name" and ctx.argv[i + 1] then
      table.insert(names, ctx.argv[i + 1]); i = i + 2
    else i = i + 1 end
  end
  return { kind = "TextSearch", node = {
    at = "local:", roots = { root }, pattern = pattern, name = names, ignoreCase = icase } }
end
)LUAU";

std::optional<kuli::luau::ScriptureSpec> builtin_spec(const std::string& name) {
    kuli::luau::ScriptureSpec s;
    s.version = "0.1.0";
    if (name == "find") {
        s.basenames["find"] = "adapters/find.luau";
        s.files["adapters/find.luau"] = kFindAdapter;
    } else if (name == "grep") {
        s.basenames["grep"] = "adapters/grep.luau";
        s.files["adapters/grep.luau"] = kGrepAdapter;
    } else {
        return std::nullopt;
    }
    return s;
}

// ----- scripture registry (~/.config/kuli/scriptures.json) ------------------
// Additive install record, independent of the bp profile generations: keyed by
// scripture name -> { storePath, version, basenames:[{alias,adapter}] }.
fs::path registry_path() { return paths::config_dir() / "scriptures.json"; }

json load_registry() {
    std::ifstream in(registry_path(), std::ios::binary);
    if (!in) return json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : json::object();
}

bool save_registry(const json& j) {
    std::error_code ec;
    fs::create_directories(registry_path().parent_path(), ec);
    std::ofstream out(registry_path(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2);
    return out.good();
}

// ----- basename resolution --------------------------------------------------
struct Found {
    std::string store_path;  // the scripture's store dir (relative to the store root)
    std::string adapter;     // adapter path within that store dir
};

std::optional<Found> find_basename(const Generation& gen, const std::string& name) {
    for (const auto& d : gen.derivations) {
        for (const auto& b : d.basenames) {
            if (b.alias == name) return Found{d.store_path, b.adapter};
        }
    }
    return std::nullopt;
}

// Installed scriptures (registry) take precedence, then bp-apply'd scriptures
// (the current generation).
std::optional<Found> resolve_basename(const std::string& name) {
    json reg = load_registry();
    for (const auto& [_, e] : reg.items()) {
        for (const auto& b : e.value("basenames", json::array())) {
            if (b.value("alias", "") == name) {
                return Found{e.value("storePath", ""), b.value("adapter", "")};
            }
        }
    }
    if (auto cur = default_profile().current()) {
        if (auto f = find_basename(*cur, name)) return f;
    }
    return std::nullopt;
}

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

}  // namespace

std::vector<std::string> builtin_scripture_names() { return {"find", "grep"}; }

bool is_installed_basename(const std::string& basename) {
    return resolve_basename(basename).has_value();
}

int run_basename(const std::string& basename, const std::vector<std::string>& argv,
                 const fs::path& cwd) {
    auto found = resolve_basename(basename);
    if (!found) {
        return report(kuli::diag::Diagnostic::error(
            "no installed scripture provides the basename '" + basename + "'", "E0961"));
    }

    fs::path scripture_root = paths::store_dir() / found->store_path;
    kuli::luau::AdapterRequest req;
    req.adapter_path = scripture_root / fs::path(found->adapter);
    req.scripture_root = scripture_root;
    req.argv = argv;
    req.system = kuli::luau::SystemInfo{std::string(kuli::platform::host_os()),
                                        std::string(kuli::platform::host_arch()), ""};

    auto res = kuli::luau::evaluate_adapter(req);
    if (!res) return report(res.error());
    const json& v = res->value;

    // IR-node form `{ kind = "...", node = {...} }` — wrap in the envelope and
    // dispatch to the engine (§9.2: adapter -> IR -> engine.execute -> render).
    if (v.is_object() && v.contains("kind") && v["kind"].is_string()) {
        json ir;
        ir["schema"] = std::string(kuli::ir::SCHEMA);
        ir["kind"] = v["kind"];
        ir["node"] = v.value("node", json::object());

        kuli::engine::Engine engine;
        kuli::engine::AdapterCall call;
        call.tool_name = basename;
        call.cwd = cwd;
        call.ir_doc = std::move(ir);
        kuli::engine::RawResult rr = engine.execute(call);
        for (const auto& line : rr.lines) std::cout << line << "\n";
        if (!rr.raw_stderr.empty()) std::cerr << rr.raw_stderr;
        return rr.exit_code;
    }

    // Pure text form `{ lines = { "..." } }`.
    if (v.is_object() && v.contains("lines") && v["lines"].is_array()) {
        for (const auto& l : v["lines"]) {
            if (l.is_string()) std::cout << l.get<std::string>() << "\n";
        }
        return 0;
    }

    return report(kuli::diag::Diagnostic::error(
        "scripture '" + basename + "' returned neither { lines } nor an IR { kind }", "E0962"));
}

int install_builtin_scripture(const std::string& name) {
    auto spec = builtin_spec(name);
    if (!spec) {
        return report(kuli::diag::Diagnostic::error("unknown built-in scripture: " + name, "E0970")
                          .with_help("available: find, grep"));
    }
    std::string hash = kuli::luau::hash_scripture(name, *spec);
    std::string store_path = kuli::luau::store_path_for_scripture(hash, name);

    // Realize into the content store (manifest.json + adapters/*), like the
    // engine does for a mkScripture derivation.
    std::vector<std::pair<std::string, std::string>> files;
    for (const auto& [rel, content] : spec->files) files.emplace_back(rel, content);
    json basenames_j = json::object();
    for (const auto& [alias, rel] : spec->basenames) basenames_j[alias] = rel;
    json manifest{{"name", name}, {"version", spec->version}, {"basenames", basenames_j}};
    files.emplace_back("manifest.json", manifest.dump(2));

    kuli::store::Store st{paths::store_dir(), paths::downloads_dir()};
    auto rr = st.realize_inline(store_path, hash, files);
    if (!rr) return report(rr.error());

    // Write the basename shims (kuli --basename <alias>) and put ~/.local/bin on
    // PATH (honors KULI_SKIP_PATH).
    fs::path bin = paths::xdg_bin_home();
    fs::path exe = paths::current_exe();
    for (const auto& [alias, rel] : spec->basenames) {
        if (!kuli::platform::write_basename_shim(bin, alias, exe)) {
            return report(kuli::diag::Diagnostic::error(
                "cannot write basename shim for '" + alias + "'", "E0971"));
        }
    }
    kuli::platform::hkcu_path_prepend("~/.local/bin");

    // Record in the registry (additive).
    json reg = load_registry();
    json bn = json::array();
    std::string alias_list;
    for (const auto& [alias, rel] : spec->basenames) {
        bn.push_back({{"alias", alias}, {"adapter", rel}});
        alias_list += (alias_list.empty() ? "" : ", ") + alias;
    }
    reg[name] = {{"name", name}, {"storePath", store_path}, {"version", spec->version},
                 {"basenames", bn}};
    if (!save_registry(reg)) {
        return report(kuli::diag::Diagnostic::error("cannot write scripture registry", "E0972"));
    }
    std::cout << "installed scripture '" << name << "' (" << alias_list << ") -> " << bin.string()
              << "\n";
    return 0;
}

int uninstall_scripture(const std::string& name) {
    json reg = load_registry();
    if (!reg.contains(name)) {
        return report(kuli::diag::Diagnostic::error(
            "scripture '" + name + "' is not installed", "E0973"));
    }
    fs::path bin = paths::xdg_bin_home();
    for (const auto& b : reg[name].value("basenames", json::array())) {
        kuli::platform::remove_shim(bin, b.value("alias", ""));
    }
    reg.erase(name);
    if (!save_registry(reg)) {
        return report(kuli::diag::Diagnostic::error("cannot write scripture registry", "E0972"));
    }
    std::cout << "uninstalled scripture '" << name << "'\n";
    return 0;
}

int scripture_list() {
    json reg = load_registry();
    for (const auto& [sname, e] : reg.items()) {
        std::string aliases;
        for (const auto& b : e.value("basenames", json::array())) {
            aliases += (aliases.empty() ? "" : ", ") + b.value("alias", std::string());
        }
        std::cout << sname << "  (" << aliases << ")  [installed]\n";
    }
    // bp-apply'd scriptures (current generation).
    if (auto cur = default_profile().current()) {
        for (const auto& d : cur->derivations) {
            if (d.basenames.empty()) continue;
            std::string aliases;
            for (const auto& b : d.basenames) aliases += (aliases.empty() ? "" : ", ") + b.alias;
            std::cout << d.name << "  (" << aliases << ")  [generation " << cur->id << "]\n";
        }
    }
    // Built-ins not yet installed.
    for (const auto& name : builtin_scripture_names()) {
        if (!reg.contains(name)) {
            std::cout << name << "  (built-in — `kuli scripture install " << name << "`)\n";
        }
    }
    return 0;
}

}  // namespace kuli::bp

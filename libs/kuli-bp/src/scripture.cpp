#include "kuli/bp/scripture.hpp"

#include <iostream>
#include <optional>

#include <nlohmann/json.hpp>

#include "kuli/bp/generation.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"

namespace kuli::bp {

namespace {

// Locate the (derivation, basename) the current generation installs for `name`.
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

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

}  // namespace

bool is_installed_basename(const std::string& basename) {
    auto cur = default_profile().current();
    return cur && find_basename(*cur, basename).has_value();
}

int run_basename(const std::string& basename, const std::vector<std::string>& argv,
                 const fs::path& cwd) {
    auto cur = default_profile().current();
    if (!cur) {
        return report(kuli::diag::Diagnostic::error(
            "no active generation, so no scripture provides '" + basename + "'", "E0960"));
    }
    auto found = find_basename(*cur, basename);
    if (!found) {
        return report(kuli::diag::Diagnostic::error(
            "no installed scripture provides the basename '" + basename + "'", "E0961"));
    }

    fs::path store_root = kuli::platform::paths::store_dir();
    fs::path scripture_root = store_root / found->store_path;

    kuli::luau::AdapterRequest req;
    req.adapter_path = scripture_root / fs::path(found->adapter);
    req.scripture_root = scripture_root;
    req.argv = argv;
    req.system = kuli::luau::SystemInfo{std::string(kuli::platform::host_os()),
                                        std::string(kuli::platform::host_arch()), ""};

    auto res = kuli::luau::evaluate_adapter(req);
    if (!res) return report(res.error());
    const nlohmann::json& v = res->value;

    // IR-node form `{ kind = "...", node = {...} }` — wrap in the envelope and
    // dispatch to the engine (§9.2: adapter -> IR -> engine.execute -> render).
    if (v.is_object() && v.contains("kind") && v["kind"].is_string()) {
        nlohmann::json ir;
        ir["schema"] = std::string(kuli::ir::SCHEMA);
        ir["kind"] = v["kind"];
        ir["node"] = v.value("node", nlohmann::json::object());

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

int scripture_list() {
    auto cur = default_profile().current();
    if (!cur) return 0;
    for (const auto& d : cur->derivations) {
        if (d.basenames.empty()) continue;
        std::cout << d.name << "  (";
        for (std::size_t i = 0; i < d.basenames.size(); ++i) {
            std::cout << (i ? ", " : "") << d.basenames[i].alias;
        }
        std::cout << ")\n";
    }
    return 0;
}

}  // namespace kuli::bp

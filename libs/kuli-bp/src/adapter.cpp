#include <set>
#include <string>
#include <vector>

#include "kuli/bp/apply.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/luau/frontend.hpp"

namespace kuli::bp {

using kuli::luau::Builder;
using kuli::luau::Derivation;
using kuli::luau::DerivationGraph;
using nlohmann::json;

namespace {

// Post-order DFS: dependencies before dependents, deduplicated.
void dfs(const DerivationGraph& g, const std::string& h, std::set<std::string>& seen,
         std::vector<std::string>& order) {
    if (h.empty() || seen.count(h)) return;
    seen.insert(h);
    const Derivation* d = g.find(h);
    if (!d) return;
    if (d->builder == Builder::Composite) {
        for (const auto& c : d->requires_) dfs(g, c, seen, order);
        for (const auto& c : d->components) dfs(g, c, seen, order);
    } else if (d->builder == Builder::WithFiles) {
        dfs(g, d->base, seen, order);
    }
    order.push_back(h);
}

json deriv_json(const Derivation& d) {
    json j{
        {"hash", d.hash},
        {"name", d.name},
        {"storePath", d.store_path},
        {"builder", kuli::luau::builder_name(d.builder)},
        {"systemTarget", d.system_target},
    };
    if (d.builder == Builder::Fetch) {
        j["fetch"] = {
            {"owner", d.fetch.owner},
            {"repo", d.fetch.repo},
            {"version", d.fetch.version},
            {"assetPattern", d.fetch.asset_pattern},
            {"sha256", d.fetch.sha256},
            {"bin", d.fetch.bin},
            {"shimDir", d.fetch.shim_dir},
            {"postInstall", d.fetch.post_install},
        };
    } else if (d.builder == Builder::Composite) {
        j["components"] = d.components;
        j["requires"] = d.requires_;
    } else if (d.builder == Builder::WithFiles) {
        j["base"] = d.base;
        json files = json::array();
        for (const auto& f : d.files) {
            files.push_back({{"path", f.path}, {"mode", f.mode}, {"content", f.content}});
        }
        j["files"] = files;
    } else if (d.builder == Builder::Scripture) {
        json basenames = json::object();
        for (const auto& [alias, rel] : d.scripture.basenames) basenames[alias] = rel;
        json files = json::array();
        for (const auto& [path, content] : d.scripture.files) {
            files.push_back({{"path", path}, {"content", content}});
        }
        j["scripture"] = {{"version", d.scripture.version},
                          {"basenames", basenames},
                          {"files", files}};
    }
    return j;
}

}  // namespace

json to_apply_ir(const DerivationGraph& g, bool dry_run) {
    std::set<std::string> seen;
    std::vector<std::string> order;
    dfs(g, g.root, seen, order);

    json derivs = json::array();
    for (const auto& h : order) {
        if (const Derivation* d = g.find(h)) derivs.push_back(deriv_json(*d));
    }

    json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::ApplyDerivation);
    ir["options"] = {{"dry_run", dry_run}};
    ir["node"] = {{"root", g.root}, {"derivations", derivs}};
    ir["descent_trace"] = json::array();
    ir["sources"] = json::array();
    return ir;
}

}  // namespace kuli::bp

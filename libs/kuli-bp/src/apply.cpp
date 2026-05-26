#include "kuli/bp/apply.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "kuli/bp/generation.hpp"
#include "kuli/bp/resolver.hpp"
#include "kuli/bp/source.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/platform/win_env.hpp"

namespace kuli::bp {

namespace {

using kuli::luau::Builder;
using kuli::luau::DerivationGraph;

// Infer the bp-source root for a single blueprint file (mirrors the layout in
// design §8.1): a file under blueprints/ has the parent of that dir as root.
kuli::luau::SourceCtx source_ctx_for(const fs::path& file) {
    fs::path dir = file.parent_path();
    fs::path root = (dir.filename() == "blueprints") ? dir.parent_path() : dir;
    return kuli::luau::SourceCtx{root.filename().string(), root, ""};
}

kuli::luau::EvalRequest req_for(const fs::path& file) {
    kuli::luau::EvalRequest req;
    req.blueprint_path = file;
    req.system = kuli::luau::SystemInfo{std::string(kuli::platform::host_os()),
                                        std::string(kuli::platform::host_arch()), ""};
    req.source = source_ctx_for(file);
    return req;
}

void print_tree(const DerivationGraph& g, const std::string& hash, const std::string& prefix,
                bool last, bool is_root) {
    const auto* d = g.find(hash);
    if (!d) return;
    std::string connector = is_root ? "" : (last ? "\xE2\x94\x94\xE2\x94\x80 " : "\xE2\x94\x9C\xE2\x94\x80 ");
    std::cout << prefix << connector << d->name << "  (" << kuli::luau::builder_name(d->builder)
              << ")  " << hash.substr(0, 12);
    if (d->builder == Builder::Fetch) {
        std::cout << "  " << d->fetch.owner << "/" << d->fetch.repo << "@" << d->fetch.version;
    }
    std::cout << "\n";

    std::vector<std::string> kids;
    if (d->builder == Builder::Composite) {
        kids.insert(kids.end(), d->requires_.begin(), d->requires_.end());
        kids.insert(kids.end(), d->components.begin(), d->components.end());
    } else if (d->builder == Builder::WithFiles) {
        kids.push_back(d->base);
    }
    std::string child_prefix = is_root ? "" : prefix + (last ? "   " : "\xE2\x94\x82  ");
    for (std::size_t i = 0; i < kids.size(); ++i) {
        print_tree(g, kids[i], child_prefix, i + 1 == kids.size(), false);
    }
}

int report_eval_error(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

// Minimal kuli.lock (design §8.2.9): a reproducibility record of the realized
// derivation set. Write-only for now (consumed later by `profile diff`).
void write_lock(const DerivationGraph& g) {
    fs::path lock = kuli::platform::paths::lock_path();
    std::error_code ec;
    fs::create_directories(lock.parent_path(), ec);
    std::ofstream out(lock, std::ios::binary | std::ios::trunc);
    if (!out) return;
    out << "# kuli.lock — reproducibility record (generated)\n";
    out << "derivation_set_hash = \"" << derivation_set_hash(g) << "\"\n\n";
    for (const auto& [hash, d] : g.nodes) {
        out << "[[derivation]]\n";
        out << "hash = \"" << d.hash << "\"\n";
        out << "name = \"" << d.name << "\"\n";
        out << "storePath = \"" << d.store_path << "\"\n";
        if (d.builder == Builder::Fetch) {
            out << "source = \"github:" << d.fetch.owner << "/" << d.fetch.repo << "@"
                << d.fetch.version << "\"\n";
            out << "source_sha256 = \"" << d.fetch.sha256 << "\"\n";
        }
        out << "\n";
    }
}

int run_engine(const nlohmann::json& ir, const fs::path& cwd) {
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "kuli-bp";
    call.cwd = cwd;
    call.ir_doc = ir;
    kuli::engine::RawResult res = engine.execute(call);
    for (const auto& line : res.lines) std::cout << line << "\n";
    if (!res.raw_stderr.empty()) std::cerr << res.raw_stderr;
    return res.exit_code;
}

}  // namespace

int explain(const std::string& spec) {
    auto file = resolve_blueprint_spec(spec);
    if (!file) return report_eval_error(file.error());
    auto r = kuli::luau::evaluate(req_for(*file));
    if (!r) return report_eval_error(r.error());

    std::cout << "derivation tree for " << file->filename().string() << ":\n\n";
    print_tree(r->graph, r->graph.root, "", true, true);
    std::cout << "\n" << r->graph.nodes.size() << " derivation(s)\n";
    return 0;
}

int describe(const std::string& spec, bool json) {
    auto file = resolve_blueprint_spec(spec);
    if (!file) return report_eval_error(file.error());
    auto r = kuli::luau::evaluate(req_for(*file));
    if (!r) return report_eval_error(r.error());

    if (json) {
        // The ApplyDerivation IR doc is the canonical structured form.
        std::cout << to_apply_ir(r->graph, /*dry_run=*/true).dump(2) << "\n";
    } else {
        std::cout << "derivation tree for " << file->filename().string() << ":\n\n";
        print_tree(r->graph, r->graph.root, "", true, true);
        std::cout << "\n" << r->graph.nodes.size() << " derivation(s)\n";
    }
    return 0;
}

int apply(const std::string& spec, const ApplyOptions& opts) {
    auto file = resolve_blueprint_spec(spec);
    if (!file) return report_eval_error(file.error());
    auto r = kuli::luau::evaluate(req_for(*file));
    if (!r) return report_eval_error(r.error());
    const auto& graph = r->graph;

    // --dry-run: preview the plan, write only the evidence session, no profile.
    if (opts.dry_run) {
        return run_engine(to_apply_ir(graph, /*dry_run=*/true), opts.cwd);
    }

    // No-op detection (C1): if the realized set matches the current generation,
    // nothing changes — don't realize, don't append a generation.
    Profile profile = default_profile();
    std::string set_hash = derivation_set_hash(graph);
    if (auto cur = profile.current(); cur && cur->set_hash == set_hash) {
        std::cout << "already up to date (generation " << cur->id << ")\n";
        return 0;
    }

    nlohmann::json ir = to_apply_ir(graph, /*dry_run=*/false);
    if (auto rr = resolve_urls(ir); !rr) return report_eval_error(rr.error());

    int code = run_engine(ir, opts.cwd);  // realize into the store
    if (code != 0) return code;

    // Commit a new generation (C2), project it onto ~/.local/bin (write/remove
    // shims, move `current`), then wire PATH once (idempotent; honors
    // KULI_SKIP_PATH).
    Generation gen = profile.commit(graph);
    if (!profile.activate(gen.id, kuli::platform::paths::xdg_bin_home(),
                          kuli::platform::paths::store_dir())) {
        return report_eval_error(
            kuli::diag::Diagnostic::of(kuli::diag::Kind::Internal,
                                       "failed to project generation " + std::to_string(gen.id) +
                                           " onto ~/.local/bin (shims not fully written)",
                                       "E0700")
                .with_help("re-run `kuli bp apply` or `kuli bp doctor` to repair"));
    }
    kuli::platform::hkcu_path_prepend("~/.local/bin");
    write_lock(graph);

    std::cout << "switched to generation " << gen.id << " (" << graph.nodes.size()
              << " derivation(s))\n";
    return 0;
}

}  // namespace kuli::bp

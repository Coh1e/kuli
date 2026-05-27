#pragma once
// kuli-bp — blueprint orchestration. Loads + evaluates a blueprint (kuli-luau),
// adapts the derivation graph into an ApplyDerivation IR document, and drives
// the engine. The CLI front-end just routes here.

#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "kuli/luau/derivation.hpp"

namespace kuli::bp {

namespace fs = std::filesystem;

// Serialize a derivation graph to an ApplyDerivation IR doc. Derivations are
// emitted in topological order (dependencies first) so the engine can realize
// them in array order.
nlohmann::json to_apply_ir(const kuli::luau::DerivationGraph& graph, bool dry_run);

struct ApplyOptions {
    bool dry_run = false;
    fs::path cwd = fs::current_path();
};

// `kuli bp explain <spec>` — print the derivation tree (no realize). `spec` is
// a .luau file path or a blueprint name resolved via the source registry.
int explain(const std::string& spec);

// `kuli bp apply <spec> [--dry-run]` — evaluate -> IR -> engine. `spec` is a
// .luau file path or a blueprint name resolved via the source registry.
int apply(const std::string& spec, const ApplyOptions& opts);

// `kuli bp doctor` — check the current generation's store paths + shims.
int doctor();

// Generation verbs (operate on the default profile).
int generation_list();
int generation_rollback(int steps);
int generation_switch(int id);
int generation_diff(int a, int b);

// `kuli bp src ...` (operate on the default registry).
int src_add(const std::string& url_or_path, const std::string& name, bool assume_yes);
int src_remove(const std::string& name);
int src_update(const std::string& name);  // empty = all
int src_list();

// `kuli bp describe <spec> [--json]`.
int describe(const std::string& spec, bool json);

// `kuli run-ir <file>` — execute a kuli/ir/1.0 document read from a file. This
// is the agent entry point a transport invokes on the far side (the local
// subprocess now; ssh later). Prints the result; returns the exit code.
int run_ir(const fs::path& file, const fs::path& cwd);

}  // namespace kuli::bp

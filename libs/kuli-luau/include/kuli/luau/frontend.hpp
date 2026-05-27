#pragma once
// kuli-luau — the ONLY module that embeds Luau. This public header is
// deliberately luau-free (std + kuli-diag only); the entire Luau C/C++ API is
// confined to src/luau_frontend.cpp (R-NF-03: "core never includes luau.h;
// binding converges to a single TU").
//
// Phase B scope: sandboxed evaluation of a `return function(ctx) ... end`
// blueprint, the three hard rules (§8.1), and the §8.2.3 sandbox. Phase D
// extends EvalResult into a full DerivationGraph + ApplyDerivation IR.

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"
#include "kuli/luau/derivation.hpp"

namespace kuli::luau {

// ctx.system — host facts a blueprint may branch on.
struct SystemInfo {
    std::string os;          // "windows" / "linux" / "macos"
    std::string arch;        // "x64" / "arm64" / ...
    std::string win_version; // best-effort; empty off Windows
};

// ctx.source — the blueprint source this evaluation belongs to. `locked_at`
// is readable by blueprint logic but MUST NOT influence any hash (§8.2.1).
struct SourceCtx {
    std::string name;
    std::filesystem::path root;
    std::string locked_at;
};

// Resource ceilings for evaluating a (possibly untrusted, remote) blueprint —
// bound wall-clock time and heap so a hostile/buggy blueprint can't hang or OOM
// the process (H-1). Defaults are generous; tests tighten them.
struct EvalLimits {
    int timeout_ms = 10000;                              // wall-clock budget
    std::size_t mem_cap_bytes = std::size_t(512) << 20;  // 512 MiB heap ceiling
};

struct EvalRequest {
    // Exactly one input is used: if `inline_source` is non-empty it is
    // evaluated under `chunk_name`; otherwise `blueprint_path` is read.
    std::filesystem::path blueprint_path;
    std::string inline_source;
    std::string chunk_name = "blueprint";

    SystemInfo system;
    SourceCtx source;
    EvalLimits limits;
};

// The derivation graph the blueprint produced. `graph.root` is the hash of the
// derivation the blueprint function returned; `graph.nodes` holds it and every
// transitive dependency it composed.
struct EvalResult {
    DerivationGraph graph;

    // Convenience: the root derivation, or nullptr if (impossibly) absent.
    const Derivation* root() const { return graph.find(graph.root); }
};

// Evaluate a blueprint in a fresh sandboxed Luau VM. On any failure returns a
// Diagnostic whose `kind` the caller maps to an exit code (sandbox→3, etc.).
std::expected<EvalResult, kuli::diag::Diagnostic> evaluate(const EvalRequest& req);

// ----- scripture adapter execution (§9.2) ----------------------------------
// Running an installed scripture basename: load its adapter .luau from the
// store and call `function(ctx)` with ctx.argv = the command-line arguments.
// Same sandbox as a blueprint (R-NF-03); ctx.lib.readResource reads files under
// the scripture's own store path. The adapter returns a Luau table that is
// converted to JSON verbatim (`value`). Two shapes are meaningful to the caller:
//   * `{ lines = { "..." } }`            — pure text, printed directly
//   * `{ kind = "FileQuery", node = {} }` — an IR node dispatched to the engine
struct AdapterRequest {
    std::filesystem::path adapter_path;    // the adapter .luau inside the store
    std::filesystem::path scripture_root;  // the scripture store path (readResource base)
    std::vector<std::string> argv;         // args after the basename
    SystemInfo system;
    EvalLimits limits;
};

struct AdapterResult {
    nlohmann::json value;  // the adapter's returned table, converted to JSON
};

std::expected<AdapterResult, kuli::diag::Diagnostic> evaluate_adapter(const AdapterRequest& req);

}  // namespace kuli::luau

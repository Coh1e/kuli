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

}  // namespace kuli::luau

#pragma once
// meshell (§10.1) — a bash-flavored, mesh-native one-liner. A line is a
// `|`-separated list of stages; each stage is an optional `@target` prefix plus
// a command. It parses to a kuli/ir/1.0 doc: a single stage -> Exec, multiple
// stages -> Pipeline. Targets: `@local` -> local:, `@<alias>` -> the host
// registry (engine-resolved); bare (no @) -> local.
//
//   kuli @prod ps -ef
//   kuli mesh "@a:cat /f | @b:sort | @local:save /out"
//
// v0 tokenizes on whitespace (no quote/escape handling yet) and splits stages
// on a top-level `|`.

#include <expected>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"

namespace kuli::bp {

namespace fs = std::filesystem;

// Parse a meshell line into an IR document (Exec or Pipeline). Pure/testable.
std::expected<nlohmann::json, kuli::diag::Diagnostic> parse_meshell(const std::string& line);

// `kuli mesh "<line>"` / `kuli @host cmd ...` — parse + execute, render output.
int run_mesh(const std::string& line, const fs::path& cwd);

}  // namespace kuli::bp

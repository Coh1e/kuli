#pragma once
// kuli-ir — the kuli/ir/1.0 protocol. Net-new (luban has no IR layer); this is
// the "IR 为骨" reconciliation point. Phase E ships the ApplyDerivation family
// + a hand-written validator. EnvSet / FileOp / Exec are declared kinds the
// executor will grow into. Depends only on nlohmann-json + kuli-diag.
//
// Envelope (design §4.1): { schema, kind, node, options, descent_trace, sources }.

#include <expected>
#include <string_view>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"

namespace kuli::ir {

inline constexpr std::string_view SCHEMA = "kuli/ir/1.0";

namespace kind {
inline constexpr std::string_view ApplyDerivation = "ApplyDerivation";
inline constexpr std::string_view FileQuery = "FileQuery";    // find/fd-style read (§4.4)
inline constexpr std::string_view TextSearch = "TextSearch";  // grep/rg-style read (§4.4)
inline constexpr std::string_view EnvSet = "EnvSet";
inline constexpr std::string_view FileOp = "FileOp";
inline constexpr std::string_view Exec = "Exec";
}  // namespace kind

// Validate an IR document against kuli/ir/1.0 (hand-written checks, first error
// wins). On failure the Diagnostic's kind is IrValidation (exit code 2).
std::expected<void, kuli::diag::Diagnostic> validate(const nlohmann::json& doc);

}  // namespace kuli::ir

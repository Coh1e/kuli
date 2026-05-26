#pragma once
// kuli-diag — rustc-style diagnostics + the canonical exit-code map.
//
// Every fallible kuli operation surfaces a `Diagnostic`; the CLI renders it to
// stderr and maps `kind` to a process exit code (docs/cli.md §7). This is a
// leaf module: it depends on nothing but the standard library so it can be
// linked everywhere.

#include <optional>
#include <string>
#include <vector>

namespace kuli::diag {

enum class Severity { Error, Warning, Note, Help };

// Drives the process exit code (docs/cli.md §7). The enum order is incidental;
// `exit_code_of` is the source of truth.
enum class Kind {
    General,           // 1  — parse / argument / business failure
    IrValidation,      // 2  — kuli/ir/1.0 schema validation failed
    Sandbox,           // 3  — Luau sandbox boundary violated (R-NF-03)
    RemotePeer,        // 4  — remote peer error / unreachable
    NoCandidate,       // 5  — scheduler found no peer (fallback=fail)
    UserCancel,        // 6  — user declined a dangerous operation
    SessionInterrupt,  // 7  — session interrupted (resumable)
    Internal,          // 70 — internal bug; should be reported
};

// A source location with an attached label, rustc-style. `file` may be a
// blueprint path; `line`/`col` are 1-based (0 = unknown).
struct Span {
    std::string file;
    int line = 0;
    int col = 0;
    std::string label;
};

struct Diagnostic {
    Kind kind = Kind::General;
    Severity severity = Severity::Error;
    std::string code;     // e.g. "E0301"; empty if uncoded
    std::string message;
    std::vector<Span> spans;
    std::optional<std::string> help;

    static Diagnostic error(std::string message, std::string code = {}) {
        return Diagnostic{Kind::General, Severity::Error, std::move(code),
                          std::move(message), {}, std::nullopt};
    }
    static Diagnostic of(Kind kind, std::string message, std::string code = {}) {
        return Diagnostic{kind, Severity::Error, std::move(code),
                          std::move(message), {}, std::nullopt};
    }

    Diagnostic& with_help(std::string h) {
        help = std::move(h);
        return *this;
    }
    Diagnostic& with_span(Span s) {
        spans.push_back(std::move(s));
        return *this;
    }
};

// Map a diagnostic kind / diagnostic to its process exit code (docs/cli.md §7).
[[nodiscard]] int exit_code_of(Kind kind) noexcept;
[[nodiscard]] int exit_code_of(const Diagnostic& d) noexcept;

// Render a diagnostic for a human. `color` gates ANSI escapes (off when the
// stream is not a TTY or --no-color). Output mirrors rustc:
//   error[E0301]: <message>
//     --> <file>:<line>:<col>
//      = help: <help>
[[nodiscard]] std::string render(const Diagnostic& d, bool color = true);

}  // namespace kuli::diag

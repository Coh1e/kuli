#include "kuli/diag/diagnostic.hpp"

#include <sstream>

namespace kuli::diag {

namespace {
constexpr const char* kReset = "\x1b[0m";
constexpr const char* kBoldRed = "\x1b[1;31m";
constexpr const char* kBoldYellow = "\x1b[1;33m";
constexpr const char* kBoldBlue = "\x1b[1;34m";
constexpr const char* kBold = "\x1b[1m";

const char* severity_word(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
        case Severity::Help:    return "help";
    }
    return "error";
}

const char* severity_color(Severity s) {
    switch (s) {
        case Severity::Error:   return kBoldRed;
        case Severity::Warning: return kBoldYellow;
        case Severity::Note:    return kBoldBlue;
        case Severity::Help:    return kBoldBlue;
    }
    return kBoldRed;
}
}  // namespace

int exit_code_of(Kind kind) noexcept {
    switch (kind) {
        case Kind::General:          return 1;
        case Kind::IrValidation:     return 2;
        case Kind::Sandbox:          return 3;
        case Kind::RemotePeer:       return 4;
        case Kind::NoCandidate:      return 5;
        case Kind::UserCancel:       return 6;
        case Kind::SessionInterrupt: return 7;
        case Kind::Internal:         return 70;
    }
    return 1;
}

int exit_code_of(const Diagnostic& d) noexcept { return exit_code_of(d.kind); }

std::string render(const Diagnostic& d, bool color) {
    const char* sev_col = color ? severity_color(d.severity) : "";
    const char* bold = color ? kBold : "";
    const char* reset = color ? kReset : "";

    std::ostringstream out;
    // error[E0301]: message
    out << sev_col << severity_word(d.severity);
    if (!d.code.empty()) out << '[' << d.code << ']';
    out << reset << bold << ": " << d.message << reset << '\n';

    // --> file:line:col   (per span)
    for (const auto& s : d.spans) {
        if (s.file.empty() && s.line == 0) {
            if (!s.label.empty()) out << "      = " << s.label << '\n';
            continue;
        }
        out << "  --> " << s.file;
        if (s.line > 0) {
            out << ':' << s.line;
            if (s.col > 0) out << ':' << s.col;
        }
        if (!s.label.empty()) out << "  " << s.label;
        out << '\n';
    }

    if (d.help) {
        out << "      = " << (color ? kBoldBlue : "") << "help" << reset << ": "
            << *d.help << '\n';
    }
    return out.str();
}

}  // namespace kuli::diag

#include "kuli/ir/ir.hpp"

#include <cctype>
#include <set>
#include <string>

namespace kuli::ir {

namespace {

using kuli::diag::Diagnostic;
using kuli::diag::Kind;
using nlohmann::json;

Diagnostic invalid(std::string msg) {
    return Diagnostic::of(Kind::IrValidation, std::move(msg), "E0200");
}

bool is_str(const json& j, const char* key) {
    return j.contains(key) && j[key].is_string();
}

bool is_sha256_hex(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

std::expected<void, Diagnostic> validate_apply_derivation(const json& node) {
    if (!node.is_object()) return std::unexpected(invalid("node must be an object"));
    if (!is_str(node, "root")) return std::unexpected(invalid("ApplyDerivation.node.root must be a string"));
    if (!node.contains("derivations") || !node["derivations"].is_array()) {
        return std::unexpected(invalid("ApplyDerivation.node.derivations must be an array"));
    }

    static const std::set<std::string> builders = {"fetch", "composite", "withFiles", "scripture"};
    std::set<std::string> hashes;
    for (const auto& d : node["derivations"]) {
        if (!d.is_object()) return std::unexpected(invalid("each derivation must be an object"));
        for (const char* f : {"hash", "name", "storePath", "builder"}) {
            if (!is_str(d, f)) {
                return std::unexpected(invalid(std::string("derivation missing string field '") + f + "'"));
            }
        }
        std::string builder = d["builder"];
        if (!builders.count(builder)) {
            return std::unexpected(invalid("unknown builder '" + builder + "'"));
        }
        if (builder == "fetch") {
            if (!d.contains("fetch") || !d["fetch"].is_object()) {
                return std::unexpected(invalid("fetch derivation missing 'fetch' object"));
            }
            const auto& f = d["fetch"];
            for (const char* k : {"owner", "repo", "version", "assetPattern", "sha256"}) {
                if (!is_str(f, k)) {
                    return std::unexpected(invalid(std::string("fetch missing string field '") + k + "'"));
                }
            }
            if (!is_sha256_hex(f["sha256"].get<std::string>())) {
                return std::unexpected(invalid("fetch sha256 must be 64 hex chars"));
            }
        }
        if (builder == "scripture") {
            if (!d.contains("scripture") || !d["scripture"].is_object()) {
                return std::unexpected(invalid("scripture derivation missing 'scripture' object"));
            }
            const auto& s = d["scripture"];
            if (!s.contains("basenames") || !s["basenames"].is_object() || s["basenames"].empty()) {
                return std::unexpected(invalid("scripture must declare at least one basename"));
            }
            for (const auto& [_, v] : s["basenames"].items()) {
                if (!v.is_string()) {
                    return std::unexpected(invalid("scripture basename must map to a string adapter path"));
                }
            }
            if (!s.contains("files") || !s["files"].is_array()) {
                return std::unexpected(invalid("scripture missing 'files' array"));
            }
            for (const auto& f : s["files"]) {
                if (!is_str(f, "path") || !f.contains("content") || !f["content"].is_string()) {
                    return std::unexpected(invalid("scripture file needs string 'path' and 'content'"));
                }
            }
        }
        hashes.insert(d["hash"].get<std::string>());
    }

    // Referential integrity: components/requires/base must point at known hashes,
    // and the root must be present.
    auto known = [&](const std::string& h) { return hashes.count(h) > 0; };
    for (const auto& d : node["derivations"]) {
        auto check_array = [&](const char* key) -> std::expected<void, Diagnostic> {
            if (d.contains(key) && d[key].is_array()) {
                for (const auto& h : d[key]) {
                    if (!h.is_string() || !known(h.get<std::string>())) {
                        return std::unexpected(invalid(std::string("derivation '") +
                                                       d["name"].get<std::string>() +
                                                       "' references unknown " + key + " hash"));
                    }
                }
            }
            return {};
        };
        if (auto r = check_array("components"); !r) return r;
        if (auto r = check_array("requires"); !r) return r;
        if (d.contains("base") && d["base"].is_string() && !known(d["base"].get<std::string>())) {
            return std::unexpected(invalid("withFiles derivation references unknown base hash"));
        }
    }
    if (!known(node["root"].get<std::string>())) {
        return std::unexpected(invalid("node.root is not among node.derivations"));
    }
    return {};
}

std::expected<void, Diagnostic> validate_file_query(const json& node) {
    if (!node.is_object()) return std::unexpected(invalid("FileQuery.node must be an object"));
    // `at` defaults to "local:"; only an absent or string value is accepted.
    if (node.contains("at") && !node["at"].is_string()) {
        return std::unexpected(invalid("FileQuery.node.at must be a string URI"));
    }
    if (!node.contains("roots") || !node["roots"].is_array() || node["roots"].empty()) {
        return std::unexpected(invalid("FileQuery.node.roots must be a non-empty array"));
    }
    for (const auto& r : node["roots"]) {
        if (!r.is_string()) return std::unexpected(invalid("FileQuery root must be a string"));
    }
    if (node.contains("name") && !node["name"].is_array()) {
        return std::unexpected(invalid("FileQuery.node.name must be an array of globs"));
    }
    return {};
}

std::expected<void, Diagnostic> validate_text_search(const json& node) {
    if (!node.is_object()) return std::unexpected(invalid("TextSearch.node must be an object"));
    if (node.contains("at") && !node["at"].is_string()) {
        return std::unexpected(invalid("TextSearch.node.at must be a string URI"));
    }
    if (!node.contains("roots") || !node["roots"].is_array() || node["roots"].empty()) {
        return std::unexpected(invalid("TextSearch.node.roots must be a non-empty array"));
    }
    for (const auto& r : node["roots"]) {
        if (!r.is_string()) return std::unexpected(invalid("TextSearch root must be a string"));
    }
    if (!is_str(node, "pattern") || node["pattern"].get<std::string>().empty()) {
        return std::unexpected(invalid("TextSearch.node.pattern must be a non-empty string"));
    }
    if (node.contains("name") && !node["name"].is_array()) {
        return std::unexpected(invalid("TextSearch.node.name must be an array of globs"));
    }
    return {};
}

// ProcessQuery / HostFacts share a tiny shape: an object with an optional `at`
// URI. ProcessQuery additionally allows a `name` glob array + numeric `pid`.
std::expected<void, Diagnostic> validate_simple_read(const json& node, const char* what,
                                                     bool allow_name) {
    if (!node.is_object()) {
        return std::unexpected(invalid(std::string(what) + ".node must be an object"));
    }
    if (node.contains("at") && !node["at"].is_string()) {
        return std::unexpected(invalid(std::string(what) + ".node.at must be a string URI"));
    }
    if (allow_name && node.contains("name") && !node["name"].is_array()) {
        return std::unexpected(invalid(std::string(what) + ".node.name must be an array of globs"));
    }
    if (allow_name && node.contains("pid") && !node["pid"].is_number_integer()) {
        return std::unexpected(invalid(std::string(what) + ".node.pid must be an integer"));
    }
    return {};
}

std::expected<void, Diagnostic> validate_pipeline(const json& node) {
    if (!node.is_object()) return std::unexpected(invalid("Pipeline.node must be an object"));
    if (!node.contains("stages") || !node["stages"].is_array() || node["stages"].empty()) {
        return std::unexpected(invalid("Pipeline.node.stages must be a non-empty array"));
    }
    for (const auto& s : node["stages"]) {
        if (!s.is_object() || !s.contains("cmd") || !s["cmd"].is_array() || s["cmd"].empty()) {
            return std::unexpected(invalid("each Pipeline stage needs a non-empty 'cmd' array"));
        }
        for (const auto& a : s["cmd"]) {
            if (!a.is_string()) return std::unexpected(invalid("Pipeline stage cmd entries must be strings"));
        }
        if (s.contains("at") && !s["at"].is_string()) {
            return std::unexpected(invalid("Pipeline stage 'at' must be a string"));
        }
    }
    return {};
}

std::expected<void, Diagnostic> validate_exec(const json& node) {
    if (!node.is_object()) return std::unexpected(invalid("Exec.node must be an object"));
    if (node.contains("at") && !node["at"].is_string()) {
        return std::unexpected(invalid("Exec.node.at must be a string URI"));
    }
    if (!node.contains("cmd") || !node["cmd"].is_array() || node["cmd"].empty()) {
        return std::unexpected(invalid("Exec.node.cmd must be a non-empty argv array"));
    }
    for (const auto& a : node["cmd"]) {
        if (!a.is_string()) return std::unexpected(invalid("Exec.node.cmd entries must be strings"));
    }
    if (node.contains("cwd") && !node["cwd"].is_string()) {
        return std::unexpected(invalid("Exec.node.cwd must be a string"));
    }
    if (node.contains("capture") && !node["capture"].is_boolean()) {
        return std::unexpected(invalid("Exec.node.capture must be a boolean"));
    }
    if (node.contains("stdin") && !node["stdin"].is_string()) {
        return std::unexpected(invalid("Exec.node.stdin must be a string"));
    }
    return {};
}

}  // namespace

std::expected<void, Diagnostic> validate(const json& doc) {
    if (!doc.is_object()) return std::unexpected(invalid("IR document must be a JSON object"));
    if (!is_str(doc, "schema") || doc["schema"].get<std::string>() != std::string(SCHEMA)) {
        return std::unexpected(invalid(std::string("schema must be \"") + std::string(SCHEMA) + "\""));
    }
    if (!is_str(doc, "kind")) return std::unexpected(invalid("missing string field 'kind'"));
    if (!doc.contains("node")) return std::unexpected(invalid("missing 'node'"));

    std::string k = doc["kind"];
    if (k == kind::ApplyDerivation) return validate_apply_derivation(doc["node"]);
    if (k == kind::FileQuery) return validate_file_query(doc["node"]);
    if (k == kind::TextSearch) return validate_text_search(doc["node"]);
    if (k == kind::ProcessQuery) return validate_simple_read(doc["node"], "ProcessQuery", true);
    if (k == kind::NetworkQuery) return validate_simple_read(doc["node"], "NetworkQuery", false);
    if (k == kind::HostFacts) return validate_simple_read(doc["node"], "HostFacts", false);
    if (k == kind::EnvQuery) return validate_simple_read(doc["node"], "EnvQuery", true);
    if (k == kind::Exec) return validate_exec(doc["node"]);
    if (k == kind::Pipeline) return validate_pipeline(doc["node"]);
    // Other kinds are declared but not yet validated in depth.
    if (k == kind::EnvSet || k == kind::FileOp) return {};
    return std::unexpected(invalid("unknown IR kind '" + k + "'"));
}

}  // namespace kuli::ir

#include "kuli/engine/engine.hpp"

#include <chrono>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "kuli/crypto/hash.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/store/store.hpp"

namespace kuli::engine {

namespace {

using nlohmann::json;

std::string new_session_id() {
    auto now = std::chrono::system_clock::now();
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
    static std::mt19937_64 rng{std::random_device{}()};
    char suffix[5];
    std::snprintf(suffix, sizeof(suffix), "%04x", static_cast<unsigned>(rng() & 0xffff));
    return std::to_string(secs) + "-" + suffix;
}

void write_json(const fs::path& p, const json& j) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out << s;
}

// L1 intent (the derivation DAG) -> L2 plan (ordered concrete steps). The
// derivations array is already topologically sorted (dependencies first).
json expand_plan(const json& ir) {
    json plan = json::array();
    const json& node = ir.at("node");
    for (const auto& d : node.at("derivations")) {
        std::string builder = d.value("builder", "");
        plan.push_back({{"op", "realize"},
                        {"hash", d.value("hash", "")},
                        {"name", d.value("name", "")},
                        {"storePath", d.value("storePath", "")},
                        {"builder", builder}});
        if (builder == "fetch") {
            const json fetch = d.value("fetch", json::object());
            std::string bin = fetch.value("bin", "");
            std::string shim_dir = fetch.value("shimDir", "");
            if (!bin.empty()) {
                plan.push_back({{"op", "writeShim"},
                                {"name", d.value("name", "")},
                                {"target", bin}});
            }
            if (!shim_dir.empty()) {
                plan.push_back({{"op", "writeShimDir"},
                                {"name", d.value("name", "")},
                                {"dir", shim_dir}});
            }
        } else if (builder == "withFiles") {
            plan.push_back({{"op", "deployFiles"}, {"name", d.value("name", "")}});
        } else if (builder == "scripture") {
            const json sc = d.value("scripture", json::object());
            for (const auto& [alias, _] : sc.value("basenames", json::object()).items()) {
                plan.push_back({{"op", "installBasename"},
                                {"alias", alias},
                                {"name", d.value("name", "")}});
            }
        }
    }
    // PATH wiring is idempotent and applies once per profile.
    plan.push_back({{"op", "envSetPath"}, {"entry", "~/.local/bin"}});
    return plan;
}

RawResult fail(int code, const kuli::diag::Diagnostic& d, const std::string& session_id) {
    RawResult r;
    r.exit_code = code;
    r.raw_stderr = kuli::diag::render(d, /*color=*/false);
    r.session_id = session_id;
    return r;
}

}  // namespace

ExecEnv default_exec_env() {
    ExecEnv e;
    e.store_root = kuli::platform::paths::store_dir();
    e.downloads = kuli::platform::paths::downloads_dir();
    return e;
}

Engine::Engine(Mode mode) : env_(default_exec_env()), mode_(mode) {}

RawResult Engine::execute(const AdapterCall& call) {
    // 1) Validate against kuli/ir/1.0.
    if (auto v = kuli::ir::validate(call.ir_doc); !v) {
        return fail(kuli::diag::exit_code_of(v.error()), v.error(), "");
    }

    const bool dry_run =
        call.ir_doc.value("options", nlohmann::json::object()).value("dry_run", false);

    // 2) Open an evidence session under the invocation cwd.
    std::string id = new_session_id();
    fs::path sdir = call.cwd / ".kuli" / "sessions" / id;
    std::error_code ec;
    fs::create_directories(sdir / "steps", ec);

    nlohmann::json input{
        {"tool_name", call.tool_name},
        {"argv", call.argv},
        {"cwd", call.cwd.string()},
        {"dry_run", dry_run},
    };
    write_json(sdir / "input.json", input);
    write_json(sdir / "ir.json", call.ir_doc);

    // 3) Expand L1 -> L2 plan.
    nlohmann::json plan = expand_plan(call.ir_doc);
    write_json(sdir / "plan.json", plan);

    // 4) Dry-run short-circuits here: nothing else touches disk / HKCU.
    if (dry_run) {
        RawResult r;
        r.session_id = id;
        std::size_t derivs = call.ir_doc["node"]["derivations"].size();
        r.lines.push_back("dry-run: would apply " + std::to_string(derivs) + " derivation(s)");
        for (const auto& step : plan) {
            std::string op = step.value("op", "");
            if (op == "realize") {
                std::string h = step.value("hash", "");
                r.lines.push_back("  realize " + step.value("name", "") + " (" +
                                  h.substr(0, 12) + ", " + step.value("builder", "") + ")");
            } else if (op == "writeShim") {
                r.lines.push_back("  shim    " + step.value("target", ""));
            } else if (op == "writeShimDir") {
                r.lines.push_back("  shim*   " + step.value("dir", "") + "/*");
            } else if (op == "installBasename") {
                r.lines.push_back("  basename " + step.value("alias", "") + " -> kuli --basename " +
                                  step.value("alias", ""));
            } else if (op == "envSetPath") {
                r.lines.push_back("  env     PATH += " + step.value("entry", ""));
            }
        }
        std::string summary = "# dry-run\n\n" + std::to_string(derivs) +
                              " derivation(s) planned; nothing written outside this session.\n";
        write_text(sdir / "summary.md", summary);
        r.lines.push_back("evidence session: " + (sdir).string());
        r.lines.push_back("(--dry-run: nothing else written)");
        return r;
    }

    // 5) Execute the plan: realize derivations into the store, write shims,
    //    wire PATH. Derivations are topo-sorted (dependencies first).
    RawResult r;
    r.session_id = id;
    kuli::store::Store store{env_.store_root, env_.downloads};
    int step = 0;

    for (const auto& d : call.ir_doc["node"]["derivations"]) {
        std::string builder = d.value("builder", "");
        std::string name = d.value("name", "");
        std::string hash = d.value("hash", "");
        std::string hash16 = hash.substr(0, 16);
        json record{{"op", "realize"}, {"name", name}, {"hash", hash}, {"builder", builder}};

        if (builder == "fetch") {
            const json fetch = d.value("fetch", json::object());
            std::string url = fetch.value("url", "");
            std::string sha = fetch.value("sha256", "");
            std::string bin = fetch.value("bin", "");
            if (url.empty()) {
                r.exit_code = 1;
                r.raw_stderr = kuli::diag::render(
                    kuli::diag::Diagnostic::error(
                        "fetch derivation '" + name + "' has no resolved url", "E0501")
                        .with_help("the source resolver must run before execute"),
                    /*color=*/false);
                return r;
            }
            auto sp = kuli::crypto::parse(sha);
            if (!sp) {
                r.exit_code = 1;
                r.raw_stderr = kuli::diag::render(
                    kuli::diag::Diagnostic::error(
                        "malformed sha256 for fetch derivation '" + name + "'", "E0502"),
                    /*color=*/false);
                return r;
            }
            auto fr = store.realize_fetch(hash16, name, url, *sp, bin);
            if (!fr) {
                r.exit_code = kuli::diag::exit_code_of(fr.error());
                r.raw_stderr = kuli::diag::render(fr.error(), /*color=*/false);
                return r;
            }
            record["storeDir"] = fr->store_dir.string();
            record["wasPresent"] = fr->was_already_present;
            r.lines.push_back((fr->was_already_present ? "cached   " : "realized ") + name +
                              " -> " + fr->store_dir.string());
        } else if (builder == "scripture") {
            const json sc = d.value("scripture", json::object());
            std::string store_dir = d.value("storePath", "");
            std::vector<std::pair<std::string, std::string>> files;
            for (const auto& f : sc.value("files", json::array())) {
                files.emplace_back(f.value("path", ""), f.value("content", ""));
            }
            // A generated JSON manifest makes the store path self-describing (§9.1)
            // and lets the basename router read basenames without a Luau VM.
            json manifest{{"name", name},
                          {"version", sc.value("version", "")},
                          {"basenames", sc.value("basenames", json::object())}};
            files.emplace_back("manifest.json", manifest.dump(2));

            auto sr = store.realize_inline(store_dir, hash, files);
            if (!sr) {
                r.exit_code = kuli::diag::exit_code_of(sr.error());
                r.raw_stderr = kuli::diag::render(sr.error(), /*color=*/false);
                return r;
            }
            record["storeDir"] = sr->store_dir.string();
            record["wasPresent"] = sr->was_already_present;
            r.lines.push_back((sr->was_already_present ? "cached   " : "realized ") + name +
                              " (scripture) -> " + sr->store_dir.string());
        }
        // composite: children already realized above; symlinkJoin of the merged
        // dir is a Phase I refinement. withFiles: Phase I.

        write_json(sdir / "steps" / (std::to_string(step++) + ".json"), record);
    }

    write_text(sdir / "summary.md",
               "# run\n\nrealized " + std::to_string(step) + " derivation step(s).\n");
    r.lines.push_back("evidence session: " + sdir.string());
    return r;
}

}  // namespace kuli::engine

#include "kuli/engine/engine.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "kuli/crypto/hash.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/platform/process.hpp"
#include "kuli/sense/sense.hpp"
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

// The host registry written by `kuli host add` (~/.config/kuli/hosts.json):
// alias -> { transport: "ssh"|"local-subprocess", target: "user@host" }.
nlohmann::json read_hosts() {
    std::ifstream in(kuli::platform::paths::config_dir() / "hosts.json", std::ios::binary);
    if (!in) return nlohmann::json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : nlohmann::json::object();
}

std::vector<std::string> split_ws(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

// Transport (§ transport): execute an IR whose node targets a non-local `at:`.
// The target IR is rewritten to `local:` and shipped (over stdin) to a kuli on
// the far side — a child process (local-subprocess) or `ssh <host> kuli run-ir -`.
// `@alias` resolves through the host registry (incl. ssh port/identity/extra/
// remote-kuli options). Output is relayed back.
RawResult route_remote(const AdapterCall& call, std::string at) {
    std::string transport, target, port, identity, extra, remote_kuli = "kuli";
    if (!at.empty() && at[0] == '@') {  // resolve a host alias
        std::string alias = at.substr(1);
        nlohmann::json hosts = read_hosts();
        if (!hosts.contains(alias)) {
            return fail(2,
                        kuli::diag::Diagnostic::error("unknown host alias '@" + alias + "'", "E0644")
                            .with_help("register it: kuli host add " + alias + " <user@host>"),
                        "");
        }
        const auto& e = hosts[alias];
        transport = e.value("transport", std::string("ssh"));
        target = e.value("target", std::string());
        port = e.value("port", std::string());
        identity = e.value("identity", std::string());
        extra = e.value("extra", std::string());
        std::string rk = e.value("remote_kuli", std::string());
        if (!rk.empty()) remote_kuli = rk;
    } else if (at == "local-subprocess:" || at == "subprocess:") {
        transport = "local-subprocess";
    } else if (at.rfind("ssh:", 0) == 0) {
        transport = "ssh";
        target = at.substr(4);
    } else {
        return fail(2,
                    kuli::diag::Diagnostic::error("transport for at: '" + at + "' not implemented yet",
                                                  "E0640")
                        .with_help("supported: local:, local-subprocess:, ssh:<target>, @alias"),
                    "");
    }

    std::vector<std::string> spawn;
    if (transport == "local-subprocess") {
        spawn = {kuli::platform::paths::current_exe().string(), "run-ir", "-"};
    } else {  // ssh
        if (target.empty()) {
            return fail(2, kuli::diag::Diagnostic::error("ssh transport needs a target (user@host)",
                                                         "E0643"),
                        "");
        }
        spawn = {"ssh"};
        if (!port.empty()) { spawn.push_back("-p"); spawn.push_back(port); }
        if (!identity.empty()) { spawn.push_back("-i"); spawn.push_back(identity); }
        for (auto& a : split_ws(extra)) spawn.push_back(std::move(a));
        spawn.push_back(target);
        spawn.push_back(remote_kuli);  // remote PATH lookup or full path
        spawn.push_back("run-ir");
        spawn.push_back("-");
    }

    nlohmann::json ir = call.ir_doc;
    ir["node"]["at"] = "local:";  // the far side runs it locally
    kuli::platform::ProcessResult pr =
        kuli::platform::run_process(spawn, call.cwd, /*capture=*/true, /*input=*/ir.dump());
    if (!pr.launched) {
        return fail(127,
                    kuli::diag::Diagnostic::error("failed to spawn transport: " + spawn[0], "E0641"),
                    "");
    }
    RawResult r;
    r.exit_code = pr.exit_code;
    std::istringstream ss(pr.output);  // far-side stdout -> our result lines
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        r.lines.push_back(line);
    }
    r.raw_stderr = pr.error;  // far-side stderr -> our diagnostics
    return r;
}

// Classic `*` / `?` wildcard match (basename-style, case-sensitive like find).
bool glob_match(const std::string& pat, const std::string& s) {
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < s.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == s[t])) {
            ++p;
            ++t;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string ascii_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Walk node.roots (relative to cwd) collecting entries that pass the optional
// name globs / type / maxDepth filters. Shared by FileQuery + TextSearch
// (native std::filesystem, dep-free; remote `at:` URIs are a later increment).
std::vector<fs::path> collect_paths(const json& node, const fs::path& cwd,
                                    const std::string& type) {
    std::vector<std::string> names;
    for (const auto& g : node.value("name", json::array())) {
        if (g.is_string()) names.push_back(g.get<std::string>());
    }
    int max_depth = node.value("maxDepth", 0);  // 0 = unlimited
    auto name_ok = [&](const std::string& fname) {
        if (names.empty()) return true;
        for (const auto& g : names) {
            if (glob_match(g, fname)) return true;
        }
        return false;
    };

    std::vector<fs::path> out;
    for (const auto& root_j : node.value("roots", json::array())) {
        if (!root_j.is_string()) continue;
        fs::path root = root_j.get<std::string>();
        if (root.is_relative()) root = cwd / root;
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;

        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            if (max_depth > 0 && it.depth() + 1 >= max_depth) it.disable_recursion_pending();
            const fs::directory_entry& e = *it;
            std::error_code tec;
            bool is_dir = e.is_directory(tec);
            if (type == "f" && is_dir) continue;
            if (type == "d" && !is_dir) continue;
            if (!name_ok(e.path().filename().string())) continue;
            out.push_back(e.path());
        }
    }
    return out;
}

// FileQuery (§4.4): a flat read IR — emit paths matching the name/type/depth
// filters. Local filesystem only.
RawResult execute_file_query(const json& node, const fs::path& cwd) {
    std::string at = node.value("at", std::string("local:"));
    if (at != "local:" && at != "local") {
        return fail(2,
                    kuli::diag::Diagnostic::error(
                        "FileQuery 'at: " + at + "' is not supported yet (local: only)", "E0600"),
                    "");
    }
    RawResult r;
    std::string type = node.value("type", std::string("any"));  // "f" | "d" | "any"
    for (const auto& p : collect_paths(node, cwd, type)) r.lines.push_back(p.string());
    std::sort(r.lines.begin(), r.lines.end());  // stable output
    return r;
}

// TextSearch (§4.4): grep/rg-style content search — for each file under the
// roots (optionally name-filtered), emit "path:lineno:line" for matching lines.
// Literal substring by default; `regex: true` switches to std::regex
// (ECMAScript). `ignoreCase` applies to both. Binary files (a NUL byte) skip.
RawResult execute_text_search(const json& node, const fs::path& cwd) {
    std::string at = node.value("at", std::string("local:"));
    if (at != "local:" && at != "local") {
        return fail(2,
                    kuli::diag::Diagnostic::error(
                        "TextSearch 'at: " + at + "' is not supported yet (local: only)", "E0601"),
                    "");
    }
    std::string pattern = node.value("pattern", std::string());
    if (pattern.empty()) {
        return fail(2, kuli::diag::Diagnostic::error("TextSearch requires a non-empty 'pattern'",
                                                     "E0602"),
                    "");
    }
    bool icase = node.value("ignoreCase", false);
    bool use_regex = node.value("regex", false);

    std::optional<std::regex> re;
    std::string needle = icase ? ascii_lower(pattern) : pattern;
    if (use_regex) {
        try {
            auto flags = std::regex::ECMAScript;
            if (icase) flags |= std::regex::icase;
            re.emplace(pattern, flags);
        } catch (const std::regex_error& e) {
            return fail(2,
                        kuli::diag::Diagnostic::error(
                            "TextSearch invalid regex: " + std::string(e.what()), "E0603"),
                        "");
        }
    }
    auto matches = [&](const std::string& line) {
        if (re) return std::regex_search(line, *re);
        if (icase) return ascii_lower(line).find(needle) != std::string::npos;
        return line.find(needle) != std::string::npos;
    };

    RawResult r;
    std::vector<fs::path> files = collect_paths(node, cwd, "f");
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        std::string line;
        int n = 0;
        while (std::getline(in, line)) {
            if (line.find('\0') != std::string::npos) break;  // binary -> skip the file
            if (!line.empty() && line.back() == '\r') line.pop_back();
            ++n;
            if (matches(line)) {
                r.lines.push_back(f.string() + ":" + std::to_string(n) + ":" + line);
            }
        }
    }
    return r;
}

bool local_at(const json& node) {
    std::string at = node.value("at", std::string("local:"));
    return at == "local:" || at == "local";
}

// ProcessQuery (§4.4): ps-style — list processes (native via kuli-sense),
// optionally filtered by a `pid` or name globs. Output "<pid>\t<name>",
// sorted by pid.
RawResult execute_process_query(const json& node, const fs::path& /*cwd*/) {
    if (!local_at(node)) {
        return fail(2,
                    kuli::diag::Diagnostic::error("ProcessQuery is local-only for now", "E0610"),
                    "");
    }
    std::vector<std::string> names;
    for (const auto& g : node.value("name", json::array())) {
        if (g.is_string()) names.push_back(g.get<std::string>());
    }
    long want_pid = node.value("pid", 0);

    std::vector<kuli::sense::ProcessInfo> procs = kuli::sense::list_processes();
    std::sort(procs.begin(), procs.end(),
              [](const auto& a, const auto& b) { return a.pid < b.pid; });

    RawResult r;
    for (const auto& p : procs) {
        if (want_pid != 0 && p.pid != want_pid) continue;
        if (!names.empty()) {
            bool hit = false;
            for (const auto& g : names) {
                if (glob_match(g, p.name)) { hit = true; break; }
            }
            if (!hit) continue;
        }
        r.lines.push_back(std::to_string(p.pid) + "\t" + p.name);
    }
    return r;
}

// NetworkQuery (§4.4): netstat-style TCP socket listing (native via kuli-sense),
// optionally filtered to `state: "LISTEN"`. Output:
// "<proto> <local> <remote> <state> pid=<pid>".
RawResult execute_network_query(const json& node, const fs::path& /*cwd*/) {
    if (!local_at(node)) {
        return fail(2, kuli::diag::Diagnostic::error("NetworkQuery is local-only for now", "E0613"),
                    "");
    }
    std::string state_filter = node.value("state", std::string());  // e.g. "LISTEN"
    RawResult r;
    for (const auto& s : kuli::sense::list_sockets()) {
        if (!state_filter.empty() && s.state != state_filter) continue;
        r.lines.push_back(s.proto + "  " + s.local_addr + "  " + s.remote_addr + "  " + s.state +
                          "  pid=" + std::to_string(s.pid));
    }
    return r;
}

// HostFacts (§4.4): a one-shot host summary (os / arch / hostname / cpu).
RawResult execute_host_facts(const json& node, const fs::path& /*cwd*/) {
    if (!local_at(node)) {
        return fail(2, kuli::diag::Diagnostic::error("HostFacts is local-only for now", "E0611"),
                    "");
    }
    kuli::sense::HostFacts f = kuli::sense::host_facts();
    RawResult r;
    r.lines.push_back("os: " + f.os);
    r.lines.push_back("arch: " + f.arch);
    r.lines.push_back("hostname: " + f.hostname);
    r.lines.push_back("cpu: " + std::to_string(f.cpu_count));
    return r;
}

// Exec (§4.4): run a command at `at:` with stdio streamed back. Local-only for
// now (native blocking spawn via kuli-platform); --dry-run reports without
// running. The child's output goes straight to the terminal; only the exit code
// flows through RawResult.
RawResult execute_exec(const json& node, const fs::path& cwd, bool dry_run) {
    if (!local_at(node)) {
        return fail(2, kuli::diag::Diagnostic::error("Exec is local-only for now", "E0620"), "");
    }
    std::vector<std::string> argv;
    for (const auto& a : node.value("cmd", json::array())) {
        if (a.is_string()) argv.push_back(a.get<std::string>());
    }
    if (argv.empty()) {
        return fail(2, kuli::diag::Diagnostic::error("Exec.node.cmd is empty", "E0621"), "");
    }
    fs::path run_cwd = cwd;
    if (node.contains("cwd") && node["cwd"].is_string()) {
        fs::path c = node["cwd"].get<std::string>();
        run_cwd = c.is_relative() ? cwd / c : c;
    }

    if (dry_run) {
        std::string joined;
        for (const auto& a : argv) joined += (joined.empty() ? "" : " ") + a;
        RawResult r;
        r.lines.push_back("dry-run: would exec: " + joined);
        return r;
    }

    bool capture = node.value("capture", false);
    kuli::platform::ProcessResult pr = kuli::platform::run_process(argv, run_cwd, capture);
    if (!pr.launched) {
        return fail(127,
                    kuli::diag::Diagnostic::error("failed to launch: " + argv[0], "E0622")
                        .with_help("is it on PATH?"),
                    "");
    }
    RawResult r;
    r.exit_code = pr.exit_code;
    if (capture) {
        // stdout -> rendered lines, stderr -> diagnostics; without capture the
        // child already streamed both to the terminal.
        std::istringstream ss(pr.output);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            r.lines.push_back(line);
        }
        r.raw_stderr = pr.error;
    }
    return r;
}

// EnvQuery (§4.4): read the process environment, optionally filtered by name
// globs; emit "KEY=VALUE" sorted by name.
RawResult execute_env_query(const json& node, const fs::path& /*cwd*/) {
    if (!local_at(node)) {
        return fail(2, kuli::diag::Diagnostic::error("EnvQuery is local-only for now", "E0612"),
                    "");
    }
    std::vector<std::string> names;
    for (const auto& g : node.value("name", json::array())) {
        if (g.is_string()) names.push_back(g.get<std::string>());
    }
    auto vars = kuli::sense::env_vars();
    std::sort(vars.begin(), vars.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    RawResult r;
    for (const auto& [k, v] : vars) {
        if (!names.empty()) {
            bool hit = false;
            for (const auto& g : names) {
                if (glob_match(g, k)) { hit = true; break; }
            }
            if (!hit) continue;
        }
        r.lines.push_back(k + "=" + v);
    }
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

    // 1b) Transport: a non-local node `at:` routes through a transport before any
    // local execution. (ApplyDerivation has no `at` -> defaults local.)
    {
        std::string at = call.ir_doc.value("node", nlohmann::json::object())
                             .value("at", std::string("local:"));
        if (at != "local:" && at != "local" && !at.empty()) {
            return route_remote(call, at);
        }
    }

    const bool dry_run =
        call.ir_doc.value("options", nlohmann::json::object()).value("dry_run", false);

    // Flat IRs short-circuit here — no plan expansion, no evidence session. Reads
    // are side-effect-free (dry_run is a no-op for them); Exec honors dry_run.
    std::string ir_kind = call.ir_doc.value("kind", std::string{});
    auto node_of = [&] { return call.ir_doc.value("node", nlohmann::json::object()); };
    if (ir_kind == std::string(kuli::ir::kind::FileQuery)) return execute_file_query(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::TextSearch)) return execute_text_search(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::ProcessQuery)) return execute_process_query(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::NetworkQuery)) return execute_network_query(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::HostFacts)) return execute_host_facts(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::EnvQuery)) return execute_env_query(node_of(), call.cwd);
    if (ir_kind == std::string(kuli::ir::kind::Exec)) return execute_exec(node_of(), call.cwd, dry_run);

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

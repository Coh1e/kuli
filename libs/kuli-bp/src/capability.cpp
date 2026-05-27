#include "kuli/bp/capability.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include "kuli/diag/diagnostic.hpp"
#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/sense/sense.hpp"
#include "kuli/version.hpp"

namespace kuli::bp {

namespace {
using nlohmann::json;
namespace paths = kuli::platform::paths;

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

json read_json_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    return j.is_discarded() ? json::object() : j;
}

fs::path cap_dir() { return paths::config_dir() / "capabilities"; }

// Run an IR through the engine and return its result.
kuli::engine::RawResult run(const json& ir, const fs::path& cwd) {
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "capability";
    call.cwd = cwd;
    call.ir_doc = ir;
    return engine.execute(call);
}

json exec_ir(const std::string& at, const std::vector<std::string>& cmd, bool capture) {
    json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    ir["kind"] = std::string(kuli::ir::kind::Exec);
    ir["node"] = {{"at", at}, {"cmd", cmd}, {"capture", capture}};
    return ir;
}

// Parse "key op value" -> (key, op, value). op is one of = >= > <= <.
bool split_constraint(const std::string& c, std::string& key, std::string& op, std::string& val) {
    static const char* ops[] = {">=", "<=", ">", "<", "="};
    for (const char* o : ops) {
        auto pos = c.find(o);
        if (pos != std::string::npos) {
            key = c.substr(0, pos);
            op = o;
            val = c.substr(pos + std::string(o).size());
            return !key.empty();
        }
    }
    return false;
}

bool num_cmp(long lhs, const std::string& op, long rhs) {
    if (op == "=") return lhs == rhs;
    if (op == ">=") return lhs >= rhs;
    if (op == ">") return lhs > rhs;
    if (op == "<=") return lhs <= rhs;
    if (op == "<") return lhs < rhs;
    return false;
}

}  // namespace

json local_capability() {
    kuli::sense::HostFacts f = kuli::sense::host_facts();
    json scriptures = json::array();
    json reg = read_json_file(paths::config_dir() / "scriptures.json");
    if (reg.is_object()) {
        for (const auto& [name, _] : reg.items()) scriptures.push_back(name);
    }
    return json{{"host", f.hostname},
                {"os", f.os},
                {"arch", f.arch},
                {"cpu", f.cpu_count},
                {"kuli", std::string(kuli::kVersion)},
                {"scriptures", scriptures},
                {"v", 1}};  // monotonic versioning + signing deferred
}

bool capability_matches(const json& record, const std::vector<std::string>& constraints) {
    for (const auto& c : constraints) {
        std::string key, op, val;
        if (!split_constraint(c, key, op, val)) return false;
        if (key == "os" || key == "arch" || key == "host" || key == "kuli") {
            if (op != "=" || record.value(key, std::string()) != val) return false;
        } else if (key == "cpu") {
            if (!num_cmp(static_cast<long>(record.value("cpu", 0)), op,
                         std::strtol(val.c_str(), nullptr, 10)))
                return false;
        } else if (key == "scripture" || key == "has") {
            bool found = false;
            for (const auto& e : record.value("scriptures", json::array())) {
                if (e.is_string() && e.get<std::string>() == val) { found = true; break; }
            }
            if (!found) return false;
        } else {
            return false;  // unknown key
        }
    }
    return true;
}

int capability_show(bool json_out) {
    json rec = local_capability();
    if (json_out) {
        std::cout << rec.dump(2) << "\n";
        return 0;
    }
    std::cout << "host: " << rec.value("host", std::string()) << "\n";
    std::cout << "os: " << rec.value("os", std::string()) << "  arch: "
              << rec.value("arch", std::string()) << "  cpu: " << rec.value("cpu", 0) << "\n";
    std::cout << "kuli: " << rec.value("kuli", std::string()) << "\n";
    std::cout << "scriptures: ";
    for (const auto& s : rec.value("scriptures", json::array())) std::cout << s.get<std::string>() << " ";
    std::cout << "\n";
    return 0;
}

int capability_sync(const std::string& alias) {
    // Pull the peer's record by running `kuli capability --json` on it (over the
    // transport). cmd[0] = "kuli" -> must be on the peer's PATH.
    json ir = exec_ir("@" + alias, {"kuli", "capability", "--json"}, /*capture=*/true);
    kuli::engine::RawResult rr = run(ir, fs::current_path());
    if (rr.exit_code != 0) {
        if (!rr.raw_stderr.empty()) std::cerr << rr.raw_stderr;
        return report(kuli::diag::Diagnostic::error(
            "capability sync failed for @" + alias + " (exit " + std::to_string(rr.exit_code) + ")",
            "E0690"));
    }
    std::string joined;
    for (const auto& l : rr.lines) joined += l + "\n";
    json rec = json::parse(joined, nullptr, /*allow_exceptions=*/false);
    if (!rec.is_object()) {
        return report(kuli::diag::Diagnostic::error(
            "peer @" + alias + " did not return a capability record", "E0691"));
    }
    std::error_code ec;
    fs::create_directories(cap_dir(), ec);
    std::ofstream out(cap_dir() / (alias + ".json"), std::ios::binary | std::ios::trunc);
    if (!out) return report(kuli::diag::Diagnostic::error("cannot write capability cache", "E0692"));
    out << rec.dump(2);
    std::cout << "cached capability of @" << alias << " (" << rec.value("os", std::string()) << "/"
              << rec.value("arch", std::string()) << ", " << rec.value("host", std::string()) << ")\n";
    return 0;
}

int capability_list() {
    json local = local_capability();
    std::cout << "local  " << local.value("os", std::string()) << "/" << local.value("arch", std::string())
              << "  " << local.value("host", std::string()) << "\n";
    std::error_code ec;
    if (fs::exists(cap_dir(), ec)) {
        for (const auto& e : fs::directory_iterator(cap_dir(), ec)) {
            if (e.path().extension() != ".json") continue;
            json rec = read_json_file(e.path());
            std::cout << "@" << e.path().stem().string() << "  " << rec.value("os", std::string())
                      << "/" << rec.value("arch", std::string()) << "  "
                      << rec.value("host", std::string()) << "\n";
        }
    }
    return 0;
}

int route(const std::vector<std::string>& need, const std::string& fallback,
          const std::vector<std::string>& cmd, const fs::path& cwd) {
    if (cmd.empty()) {
        return report(kuli::diag::Diagnostic::error("route needs a command (-- <cmd...>)", "E0693"));
    }
    // Candidates: local first, then each cached peer.
    std::vector<std::pair<std::string, json>> candidates;
    candidates.emplace_back("local:", local_capability());
    std::error_code ec;
    if (fs::exists(cap_dir(), ec)) {
        for (const auto& e : fs::directory_iterator(cap_dir(), ec)) {
            if (e.path().extension() != ".json") continue;
            candidates.emplace_back("@" + e.path().stem().string(), read_json_file(e.path()));
        }
    }

    for (const auto& [at, rec] : candidates) {
        if (capability_matches(rec, need)) {
            std::cout << "# routed to " << (at == "local:" ? "local" : at) << " ("
                      << rec.value("host", std::string()) << ")\n"
                      << std::flush;  // before the child streams its own output
            kuli::engine::RawResult rr = run(exec_ir(at, cmd, /*capture=*/at != "local:"), cwd);
            for (const auto& l : rr.lines) std::cout << l << "\n";
            if (!rr.raw_stderr.empty()) std::cerr << rr.raw_stderr;
            return rr.exit_code;
        }
    }

    if (fallback == "local") {
        std::cout << "# no capable peer; falling back to local\n";
        kuli::engine::RawResult rr = run(exec_ir("local:", cmd, /*capture=*/false), cwd);
        return rr.exit_code;
    }
    return report(kuli::diag::Diagnostic::error("no peer satisfies the constraints", "E0694")
                      .with_help("relax --need, sync more peers, or pass --fallback local"));
}

}  // namespace kuli::bp

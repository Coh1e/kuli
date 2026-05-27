#include "kuli/bp/capability.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

#include <chrono>

#include "kuli/crypto/sign.hpp"
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

long now_epoch() {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
}

// Load the node's Ed25519 private key (PEM), generating + persisting it on first
// use at <data>/identity/ed25519.key.
std::optional<std::string> node_private_key() {
    fs::path key = paths::identity_dir() / "ed25519.key";
    if (std::ifstream in{key, std::ios::binary}) {
        std::ostringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    auto kp = kuli::crypto::ed25519_generate();
    if (!kp) return std::nullopt;
    std::error_code ec;
    fs::create_directories(key.parent_path(), ec);
    std::ofstream out(key, std::ios::binary | std::ios::trunc);
    if (!out) return std::nullopt;
    out << kp->private_pem;
    return kp->private_pem;
}

// Canonical bytes a signature covers: the record without its own `sig` field
// (nlohmann's default object is key-sorted -> deterministic).
std::string signing_bytes(json record) {
    record.erase("sig");
    return record.dump();
}

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
    json rec{{"host", f.hostname},
             {"os", f.os},
             {"arch", f.arch},
             {"cpu", f.cpu_count},
             {"kuli", std::string(kuli::kVersion)},
             {"scriptures", scriptures},
             {"v", now_epoch()}};  // monotonic (unix seconds)

    // Sign with the node identity. If the key/sign is unavailable the record is
    // still usable locally, just unsigned (sync will reject it as unverified).
    if (auto priv = node_private_key()) {
        if (auto pub = kuli::crypto::ed25519_public_of(*priv)) {
            rec["pubkey"] = *pub;
            if (auto sig = kuli::crypto::ed25519_sign(*priv, signing_bytes(rec))) {
                rec["sig"] = *sig;
            }
        }
    }
    return rec;
}

bool capability_verify(const json& record) {
    if (!record.contains("sig") || !record["sig"].is_string()) return false;
    if (!record.contains("pubkey") || !record["pubkey"].is_string()) return false;
    return kuli::crypto::ed25519_verify(record["pubkey"].get<std::string>(), signing_bytes(record),
                                        record["sig"].get<std::string>());
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
    if (!capability_verify(rec)) {
        return report(kuli::diag::Diagnostic::error(
            "capability record from @" + alias + " failed signature verification", "E0695"));
    }
    // Monotonic: never replace a cached record with an older-or-equal version.
    fs::path dst = cap_dir() / (alias + ".json");
    json cached = read_json_file(dst);
    if (cached.is_object() && cached.value("v", 0L) >= rec.value("v", 0L)) {
        std::cout << "@" << alias << " already cached at v" << cached.value("v", 0L)
                  << " (>= v" << rec.value("v", 0L) << "); keeping it\n";
        return 0;
    }
    std::error_code ec;
    fs::create_directories(cap_dir(), ec);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return report(kuli::diag::Diagnostic::error("cannot write capability cache", "E0692"));
    out << rec.dump(2);
    std::cout << "cached capability of @" << alias << " (" << rec.value("os", std::string()) << "/"
              << rec.value("arch", std::string()) << ", " << rec.value("host", std::string())
              << ", v" << rec.value("v", 0L) << ")\n";
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

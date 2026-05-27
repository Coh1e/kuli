#include "kuli/bp/hosts.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "kuli/diag/diagnostic.hpp"
#include "kuli/platform/paths.hpp"

namespace kuli::bp {

namespace {
namespace fs = std::filesystem;
using nlohmann::json;

fs::path hosts_path() { return kuli::platform::paths::config_dir() / "hosts.json"; }

json load() {
    std::ifstream in(hosts_path(), std::ios::binary);
    if (!in) return json::object();
    std::ostringstream ss;
    ss << in.rdbuf();
    auto j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    return j.is_object() ? j : json::object();
}

bool save(const json& j) {
    std::error_code ec;
    fs::create_directories(hosts_path().parent_path(), ec);
    std::ofstream out(hosts_path(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << j.dump(2);
    return out.good();
}

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}
}  // namespace

int host_add(const std::string& alias, const HostSpec& spec) {
    if (spec.transport != "ssh" && spec.transport != "local-subprocess") {
        return report(kuli::diag::Diagnostic::error(
            "transport must be 'ssh' or 'local-subprocess'", "E0980"));
    }
    if (spec.transport == "ssh" && spec.target.empty()) {
        return report(kuli::diag::Diagnostic::error("ssh host needs a target (user@host)", "E0981"));
    }
    json entry{{"transport", spec.transport}, {"target", spec.target}};
    if (!spec.port.empty()) entry["port"] = spec.port;
    if (!spec.identity.empty()) entry["identity"] = spec.identity;
    if (!spec.extra.empty()) entry["extra"] = spec.extra;
    if (!spec.remote_kuli.empty()) entry["remote_kuli"] = spec.remote_kuli;

    json h = load();
    h[alias] = entry;
    if (!save(h)) {
        return report(kuli::diag::Diagnostic::error("cannot write host registry", "E0982"));
    }
    std::cout << "registered host '" << alias << "' -> " << spec.transport
              << (spec.target.empty() ? "" : (" " + spec.target)) << "\n";
    return 0;
}

int host_remove(const std::string& alias) {
    json h = load();
    if (!h.contains(alias)) {
        return report(kuli::diag::Diagnostic::error("no such host alias: " + alias, "E0983"));
    }
    h.erase(alias);
    if (!save(h)) {
        return report(kuli::diag::Diagnostic::error("cannot write host registry", "E0982"));
    }
    std::cout << "removed host '" << alias << "'\n";
    return 0;
}

int host_list() {
    json h = load();
    for (const auto& [alias, e] : h.items()) {
        std::cout << "@" << alias << "  ->  " << e.value("transport", std::string()) << "  "
                  << e.value("target", std::string());
        if (e.contains("port")) std::cout << "  -p " << e.value("port", std::string());
        if (e.contains("identity")) std::cout << "  -i " << e.value("identity", std::string());
        if (e.contains("extra")) std::cout << "  [" << e.value("extra", std::string()) << "]";
        if (e.contains("remote_kuli")) std::cout << "  kuli=" << e.value("remote_kuli", std::string());
        std::cout << "\n";
    }
    return 0;
}

}  // namespace kuli::bp

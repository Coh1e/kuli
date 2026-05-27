#include "kuli/bp/scripture.hpp"

#include <iostream>
#include <optional>

#include "kuli/bp/generation.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/luau/frontend.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"

namespace kuli::bp {

namespace {

// Locate the (derivation, basename) the current generation installs for `name`.
struct Found {
    std::string store_path;  // the scripture's store dir (relative to the store root)
    std::string adapter;     // adapter path within that store dir
};

std::optional<Found> find_basename(const Generation& gen, const std::string& name) {
    for (const auto& d : gen.derivations) {
        for (const auto& b : d.basenames) {
            if (b.alias == name) return Found{d.store_path, b.adapter};
        }
    }
    return std::nullopt;
}

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

}  // namespace

bool is_installed_basename(const std::string& basename) {
    auto cur = default_profile().current();
    return cur && find_basename(*cur, basename).has_value();
}

int run_basename(const std::string& basename, const std::vector<std::string>& argv,
                 const fs::path& /*cwd*/) {
    auto cur = default_profile().current();
    if (!cur) {
        return report(kuli::diag::Diagnostic::error(
            "no active generation, so no scripture provides '" + basename + "'", "E0960"));
    }
    auto found = find_basename(*cur, basename);
    if (!found) {
        return report(kuli::diag::Diagnostic::error(
            "no installed scripture provides the basename '" + basename + "'", "E0961"));
    }

    fs::path store_root = kuli::platform::paths::store_dir();
    fs::path scripture_root = store_root / found->store_path;

    kuli::luau::AdapterRequest req;
    req.adapter_path = scripture_root / fs::path(found->adapter);
    req.scripture_root = scripture_root;
    req.argv = argv;
    req.system = kuli::luau::SystemInfo{std::string(kuli::platform::host_os()),
                                        std::string(kuli::platform::host_arch()), ""};

    auto res = kuli::luau::evaluate_adapter(req);
    if (!res) return report(res.error());
    for (const auto& line : res->lines) std::cout << line << "\n";
    return 0;
}

int scripture_list() {
    auto cur = default_profile().current();
    if (!cur) return 0;
    for (const auto& d : cur->derivations) {
        if (d.basenames.empty()) continue;
        std::cout << d.name << "  (";
        for (std::size_t i = 0; i < d.basenames.size(); ++i) {
            std::cout << (i ? ", " : "") << d.basenames[i].alias;
        }
        std::cout << ")\n";
    }
    return 0;
}

}  // namespace kuli::bp

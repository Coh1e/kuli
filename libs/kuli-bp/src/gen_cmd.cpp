// Generation + doctor CLI handlers (docs/cli.md §2.2 / §2.8). These operate on
// the default profile and the real XDG store / ~/.local/bin.
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "kuli/bp/apply.hpp"
#include "kuli/bp/generation.hpp"
#include "kuli/bp/source.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/platform/paths.hpp"

namespace kuli::bp {

namespace {
namespace paths = kuli::platform::paths;

int no_current() {
    std::cerr << kuli::diag::render(
        kuli::diag::Diagnostic::error("no generation applied yet", "E0600")
            .with_help("run `kuli bp apply <blueprint>` first"),
        /*color=*/false);
    return 1;
}

std::set<std::string> deriv_names(const Generation& g) {
    std::set<std::string> s;
    for (const auto& d : g.derivations) s.insert(d.name);
    return s;
}
}  // namespace

int generation_list() {
    Profile p = default_profile();
    int cur = p.current_id();
    auto ids = p.list();
    if (ids.empty()) {
        std::cout << "no generations\n";
        return 0;
    }
    for (int id : ids) {
        auto g = p.load(id);
        std::cout << (id == cur ? "* " : "  ") << id;
        if (g) std::cout << "  (" << g->derivations.size() << " derivations, parent " << g->parent << ")";
        std::cout << "\n";
    }
    return 0;
}

int generation_switch(int id) {
    Profile p = default_profile();
    if (!p.load(id)) {
        std::cerr << kuli::diag::render(
            kuli::diag::Diagnostic::error("no such generation: " + std::to_string(id), "E0601"),
            /*color=*/false);
        return 1;
    }
    if (!p.activate(id, paths::xdg_bin_home(), paths::store_dir())) return 1;
    std::cout << "switched to generation " << id << "\n";
    return 0;
}

int generation_rollback(int steps) {
    Profile p = default_profile();
    int id = p.current_id();
    if (id == 0) return no_current();
    for (int i = 0; i < steps; ++i) {
        auto g = p.load(id);
        if (!g || g->parent == 0) break;
        id = g->parent;
    }
    if (id == p.current_id()) {
        std::cout << "already at the oldest generation (" << id << ")\n";
        return 0;
    }
    if (!p.activate(id, paths::xdg_bin_home(), paths::store_dir())) return 1;
    std::cout << "rolled back to generation " << id << "\n";
    return 0;
}

int generation_diff(int a, int b) {
    Profile p = default_profile();
    auto ga = p.load(a), gb = p.load(b);
    if (!ga || !gb) {
        std::cerr << kuli::diag::render(
            kuli::diag::Diagnostic::error("generation not found", "E0601"), /*color=*/false);
        return 1;
    }
    auto na = deriv_names(*ga), nb = deriv_names(*gb);
    std::cout << "diff generation " << a << " -> " << b << ":\n";
    for (const auto& n : nb) if (!na.count(n)) std::cout << "  + " << n << "\n";
    for (const auto& n : na) if (!nb.count(n)) std::cout << "  - " << n << "\n";
    return 0;
}

// ----- bp src -------------------------------------------------------------
int src_add(const std::string& url_or_path, const std::string& name, bool assume_yes) {
    auto reg = default_registry();
    std::optional<std::string> nm =
        name.empty() ? std::nullopt : std::optional<std::string>(name);
    auto e = reg.add(url_or_path, nm, assume_yes);
    if (!e) {
        std::cerr << kuli::diag::render(e.error(), /*color=*/false);
        return kuli::diag::exit_code_of(e.error());
    }
    std::cout << "added source '" << e->name << "' -> " << e->url
              << (e->official ? "  [official]" : "  [unofficial]") << "\n";
    if (!e->is_local()) std::cout << "  synced @ " << e->commit << "\n";
    return 0;
}

int src_remove(const std::string& name) {
    auto reg = default_registry();
    if (auto r = reg.remove(name); !r) {
        std::cerr << kuli::diag::render(r.error(), /*color=*/false);
        return 1;
    }
    std::cout << "removed source '" << name << "'\n";
    return 0;
}

int src_update(const std::string& name) {
    auto reg = default_registry();
    auto names = std::vector<std::string>{};
    if (name.empty()) {
        for (const auto& e : reg.list()) names.push_back(e.name);
    } else {
        names.push_back(name);
    }
    if (names.empty()) {
        std::cout << "no sources registered\n";
        return 0;
    }
    int rc = 0;
    for (const auto& n : names) {
        auto e = reg.update(n);
        if (!e) {
            std::cerr << kuli::diag::render(e.error(), /*color=*/false);
            rc = 1;
        } else {
            std::cout << "updated '" << e->name << "' @ " << e->commit << "\n";
        }
    }
    return rc;
}

int src_list() {
    auto reg = default_registry();
    auto sources = reg.list();
    if (sources.empty()) {
        std::cout << "no sources registered\n";
        return 0;
    }
    for (const auto& e : sources) {
        std::cout << e.name << "  " << e.url << "  (" << (e.official ? "official" : "unofficial")
                  << ", " << e.commit << ")\n";
    }
    return 0;
}

int doctor() {
    Profile p = default_profile();
    auto cur = p.current();
    if (!cur) {
        std::cout << "no generation applied\n";
        return 0;
    }
    std::cout << "current generation: " << cur->id << "\n";
    bool ok = true;
    for (const auto& d : cur->derivations) {
        fs::path sp = paths::store_dir() / d.store_path;
        bool present = fs::exists(sp);
        if (!present) ok = false;
        std::cout << (present ? "  ok      " : "  MISSING ") << d.name << "  " << sp.string()
                  << "\n";
        for (const auto& s : d.shims) {
#if defined(_WIN32)
            fs::path shim = paths::xdg_bin_home() / (s.alias + ".cmd");
#else
            fs::path shim = paths::xdg_bin_home() / s.alias;
#endif
            bool sh = fs::exists(shim);
            if (!sh) ok = false;
            std::cout << (sh ? "  ok      " : "  MISSING ") << "shim " << s.alias << "\n";
        }
    }
    std::cout << (ok ? "healthy\n" : "issues found (re-apply to repair)\n");
    return ok ? 0 : 1;
}

}  // namespace kuli::bp

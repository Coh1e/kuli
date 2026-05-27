// kuli — CLI entry point. A small dispatcher over noun handlers; `kuli bp ...`
// and the `kuli-bp` basename both route through run_bp (one code path,
// docs/cli.md §1/§9.2). Blueprint verbs delegate to kuli::bp.
#include <CLI/CLI.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "kuli/bp/apply.hpp"
#include "kuli/bp/hosts.hpp"
#include "kuli/bp/scripture.hpp"
#include "kuli/diag/diagnostic.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/platform/shim.hpp"
#include "kuli/version.hpp"

namespace fs = std::filesystem;

namespace {

int print_version() {
    std::cout << "kuli " << kuli::kVersion << " (" << kuli::platform::host_triplet() << ")\n";
    return 0;
}

int report(const kuli::diag::Diagnostic& d) {
    std::cerr << kuli::diag::render(d, /*color=*/false);
    return kuli::diag::exit_code_of(d);
}

bool is_known_basename(const std::string& s) { return s == "kuli-bp"; }

// Run a CLI11 app over `args` (sub-args, no program name) by rebuilding argv.
// CLI11_PARSE returns from the enclosing function on parse error / --help.
#define KULI_PARSE(app, args)                                          \
    std::vector<char*> cargv_;                                         \
    cargv_.push_back(const_cast<char*>("kuli"));                       \
    for (const auto& a_ : (args)) cargv_.push_back(const_cast<char*>(a_.c_str())); \
    CLI11_PARSE((app), static_cast<int>(cargv_.size()), cargv_.data())

// `kuli bp ...` / `kuli-bp ...`
int run_bp(const std::vector<std::string>& args) {
    CLI::App app{"kuli bp — blueprint operations"};
    app.require_subcommand(0, 1);

    std::string explain_spec;
    auto* explain = app.add_subcommand("explain", "Show a blueprint's derivation tree");
    explain->add_option("blueprint", explain_spec, "A .luau path or a blueprint name")->required();

    std::string apply_spec;
    bool dry_run = false;
    auto* apply = app.add_subcommand("apply", "Realize a blueprint and switch profile");
    apply->add_option("blueprint", apply_spec, "A .luau path or a blueprint name")->required();
    apply->add_flag("--dry-run", dry_run, "Walk the plan without writing anything");

    std::string describe_spec;
    bool describe_json = false;
    auto* describe = app.add_subcommand("describe", "Show a blueprint's resolved derivation graph");
    describe->add_option("blueprint", describe_spec, "A .luau path or a blueprint name")->required();
    describe->add_flag("--json", describe_json, "Emit the ApplyDerivation IR as JSON");

    auto* doctor = app.add_subcommand("doctor", "Check the current generation's store + shims");

    auto* src = app.add_subcommand("src", "Manage blueprint sources (add / rm / update / ls)");
    src->require_subcommand(0, 1);
    std::string src_add_url, src_add_name;
    bool src_add_yes = false;
    auto* src_add = src->add_subcommand("add", "Register a blueprint source (github url or path)");
    src_add->add_option("source", src_add_url, "github:owner/repo, https url, or local path")->required();
    src_add->add_option("--name", src_add_name, "Override the source name");
    src_add->add_flag("--yes", src_add_yes, "Skip the trust prompt");
    std::string src_rm_name;
    auto* src_rm = src->add_subcommand("rm", "Unregister a source");
    src_rm->add_option("name", src_rm_name, "Source name")->required();
    std::string src_update_name;
    auto* src_update = src->add_subcommand("update", "Re-sync a source (or all)");
    src_update->add_option("name", src_update_name, "Source name (omit for all)");
    src->add_subcommand("ls", "List registered sources");

    KULI_PARSE(app, args);

    if (*explain) return kuli::bp::explain(explain_spec);
    if (*apply) {
        kuli::bp::ApplyOptions opts;
        opts.dry_run = dry_run;
        opts.cwd = fs::current_path();
        return kuli::bp::apply(apply_spec, opts);
    }
    if (*describe) return kuli::bp::describe(describe_spec, describe_json);
    if (*doctor) return kuli::bp::doctor();
    if (*src) {
        if (*src_add) return kuli::bp::src_add(src_add_url, src_add_name, src_add_yes);
        if (*src_rm) return kuli::bp::src_remove(src_rm_name);
        if (*src_update) return kuli::bp::src_update(src_update_name);
        return kuli::bp::src_list();  // ls or bare `bp src`
    }
    return report(kuli::diag::Diagnostic::error(
        "specify a bp verb (apply / explain / describe / doctor / src)", "E0001"));
}

// `kuli generation ...`
int run_generation(const std::vector<std::string>& args) {
    CLI::App app{"kuli generation — reversible profile history"};
    app.require_subcommand(0, 1);
    app.add_subcommand("ls", "List generations");
    int rollback_steps = 1;
    auto* rollback = app.add_subcommand("rollback", "Roll back to an earlier generation");
    rollback->add_option("--steps", rollback_steps, "How many generations to step back")->default_val(1);
    int switch_id = 0;
    auto* gswitch = app.add_subcommand("switch", "Switch to a specific generation");
    gswitch->add_option("id", switch_id, "Generation id")->required();
    int diff_a = 0, diff_b = 0;
    auto* gdiff = app.add_subcommand("diff", "Diff two generations");
    gdiff->add_option("a", diff_a, "First generation id")->required();
    gdiff->add_option("b", diff_b, "Second generation id")->required();

    KULI_PARSE(app, args);

    if (*rollback) return kuli::bp::generation_rollback(rollback_steps);
    if (*gswitch) return kuli::bp::generation_switch(switch_id);
    if (*gdiff) return kuli::bp::generation_diff(diff_a, diff_b);
    return kuli::bp::generation_list();  // ls or bare
}

// `kuli host ...` — transport aliases used by @<name> in an IR node's `at:`.
int run_host(const std::vector<std::string>& args) {
    CLI::App app{"kuli host — transport aliases for @<name>"};
    app.require_subcommand(0, 1);
    std::string add_alias, add_target, add_transport;
    auto* add = app.add_subcommand("add", "Register a host alias");
    add->add_option("alias", add_alias, "Alias (referenced as @alias)")->required();
    add->add_option("target", add_target, "ssh target user@host (omit for local-subprocess)");
    add->add_option("--transport", add_transport, "ssh | local-subprocess")->default_val("ssh");
    std::string rm_alias;
    auto* rm = app.add_subcommand("rm", "Remove a host alias");
    rm->add_option("alias", rm_alias, "Alias")->required();
    app.add_subcommand("ls", "List host aliases");

    KULI_PARSE(app, args);

    if (*add) return kuli::bp::host_add(add_alias, add_target, add_transport);
    if (*rm) return kuli::bp::host_remove(rm_alias);
    return kuli::bp::host_list();  // ls or bare
}

// `kuli scripture ...` — install / uninstall / list scriptures. Built-ins
// (find / grep) install additively; third-party packages also install via
// `kuli bp apply <scripture-blueprint>` (the unified derivation interface, §8.2.10).
int run_scripture(const std::vector<std::string>& args) {
    CLI::App app{"kuli scripture — basename packages"};
    app.require_subcommand(0, 1);
    app.add_subcommand("ls", "List installed + available scriptures");
    std::string install_name;
    auto* install = app.add_subcommand("install", "Install a built-in scripture (find/grep/kuli-bp)");
    install->add_option("name", install_name, "find / grep / kuli-bp")->required();
    std::string uninstall_name;
    auto* uninstall = app.add_subcommand("uninstall", "Remove an installed scripture");
    uninstall->add_option("name", uninstall_name, "Scripture name")->required();

    KULI_PARSE(app, args);

    if (*install) {
        // kuli-bp is the built-in bp *router* basename (a shim, not a store
        // scripture); find/grep are real bundled scriptures.
        if (install_name == "kuli-bp") {
            fs::path bin = kuli::platform::paths::xdg_bin_home();
            if (!kuli::platform::write_basename_shim(bin, "kuli-bp",
                                                     kuli::platform::paths::current_exe())) {
                return report(kuli::diag::Diagnostic::error(
                    "cannot write basename shim for kuli-bp", "E0901"));
            }
            std::cout << "installed basename 'kuli-bp' -> " << bin.string() << "\n";
            return 0;
        }
        return kuli::bp::install_builtin_scripture(install_name);
    }
    if (*uninstall) return kuli::bp::uninstall_scripture(uninstall_name);

    std::cout << "kuli-bp  (built-in router)\n";  // ls
    return kuli::bp::scripture_list();
}

}  // namespace

// Route a basename (built-in or installed scripture) to its handler.
int route_basename(const std::string& name, const std::vector<std::string>& rest) {
    if (name == "kuli-bp") return run_bp(rest);  // built-in
    return kuli::bp::run_basename(name, rest, fs::current_path());
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    // Explicit basename form used by the shims (docs/cli.md §1/§9.2):
    // `kuli --basename <name> -- <args>`.
    if (args.size() >= 2 && args[0] == "--basename") {
        std::size_t i = 2;
        if (i < args.size() && args[i] == "--") ++i;
        std::vector<std::string> rest(args.begin() + static_cast<long>(i), args.end());
        return route_basename(args[1], rest);
    }

    if (args.empty()) return print_version();
    const std::string noun = args[0];
    std::vector<std::string> sub(args.begin() + 1, args.end());

    if (noun == "version" || noun == "--version" || noun == "-V") return print_version();
    if (noun == "bp") return run_bp(sub);
    if (noun == "generation") return run_generation(sub);
    if (noun == "scripture") return run_scripture(sub);
    if (noun == "host") return run_host(sub);
    if (noun == "run-ir") {  // agent entry point: execute an IR doc from a file
        if (sub.empty()) {
            return report(kuli::diag::Diagnostic::error("run-ir needs an IR file path", "E0001"));
        }
        return kuli::bp::run_ir(sub[0], fs::current_path());
    }

    // Basename sugar: `kuli <basename> ...` for a built-in or installed scripture
    // (nouns take precedence, so this never shadows a real subcommand).
    if (is_known_basename(noun) || kuli::bp::is_installed_basename(noun)) {
        return route_basename(noun, sub);
    }

    return report(kuli::diag::Diagnostic::error("unknown command: " + noun, "E0001")
                      .with_help("try `kuli bp`, `kuli generation`, `kuli scripture`, "
                                 "`kuli host`, or `kuli version`"));
}

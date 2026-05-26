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
#include "kuli/diag/diagnostic.hpp"
#include "kuli/platform/host.hpp"
#include "kuli/platform/paths.hpp"
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

// `kuli scripture ...` — built-in basenames only (third-party packages deferred).
int run_scripture(const std::vector<std::string>& args) {
    CLI::App app{"kuli scripture — built-in basenames"};
    app.require_subcommand(0, 1);
    app.add_subcommand("ls", "List built-in basenames");
    std::string install_name;
    auto* install = app.add_subcommand("install", "Install a built-in basename shim");
    install->add_option("name", install_name, "Basename, e.g. kuli-bp")->required();

    KULI_PARSE(app, args);

    if (*install) {
        if (!is_known_basename(install_name)) {
            return report(kuli::diag::Diagnostic::error(
                "unknown built-in basename: " + install_name, "E0900"));
        }
        fs::path exe = kuli::platform::paths::current_exe();
        fs::path bin = kuli::platform::paths::xdg_bin_home();
        std::error_code ec;
        fs::create_directories(bin, ec);
        fs::path shim = bin / (install_name + ".cmd");
        std::ofstream o(shim, std::ios::binary | std::ios::trunc);
        if (!o) {
            return report(kuli::diag::Diagnostic::error("cannot write shim: " + shim.string(),
                                                        "E0901"));
        }
        o << "@echo off\r\n\"" << exe.string() << "\" --basename " << install_name << " -- %*\r\n";
        std::cout << "installed basename '" << install_name << "' -> " << shim.string() << "\n";
        return 0;
    }
    std::cout << "kuli-bp\n";  // ls
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);

    // Basename routing (docs/cli.md §1/§9.2): `kuli --basename <name> -- ...`
    // (used by the shim) or `kuli <basename> ...` sugar.
    std::string basename;
    std::vector<std::string> rest;
    if (args.size() >= 2 && args[0] == "--basename") {
        basename = args[1];
        std::size_t i = 2;
        if (i < args.size() && args[i] == "--") ++i;
        rest.assign(args.begin() + static_cast<long>(i), args.end());
    } else if (!args.empty() && is_known_basename(args[0])) {
        basename = args[0];
        rest.assign(args.begin() + 1, args.end());
    }
    if (basename == "kuli-bp") return run_bp(rest);

    if (args.empty()) return print_version();
    const std::string noun = args[0];
    std::vector<std::string> sub(args.begin() + 1, args.end());

    if (noun == "version" || noun == "--version" || noun == "-V") return print_version();
    if (noun == "bp") return run_bp(sub);
    if (noun == "generation") return run_generation(sub);
    if (noun == "scripture") return run_scripture(sub);

    return report(kuli::diag::Diagnostic::error("unknown command: " + noun, "E0001")
                      .with_help("try `kuli bp`, `kuli generation`, `kuli scripture`, or `kuli version`"));
}

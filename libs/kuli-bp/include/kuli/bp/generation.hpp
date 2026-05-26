#pragma once
// Profile / generation — the Nix-flavored, reversible activation model that
// luban lacks (it had a flat applied.txt). Each apply that changes the
// derivation set appends an immutable generation manifest; `current` is a tiny
// pointer file. Rollback re-points it (Phase H). No symlinks: the manifest is
// the source of truth, and ~/.local/bin + HKCU PATH are its projection
// (Windows symlinks need admin → would break R-NF-01).
//
// Layout: <profiles>/<name>/{ current, generations/<N>.json }

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "kuli/luau/derivation.hpp"

namespace kuli::bp {

namespace fs = std::filesystem;

// One shim a generation projects into ~/.local/bin. `bin_rel` is the path of
// the executable inside the store path, so rollback can rewrite the shim to
// point at this generation's (immutable) store path.
struct Shim {
    std::string alias;    // command name on PATH (e.g. "ninja")
    std::string bin_rel;  // exe path within the store dir (e.g. "ninja.exe")
};

struct GenDeriv {
    std::string hash;
    std::string name;
    std::string store_path;
    std::vector<Shim> shims;
    std::vector<kuli::luau::FileEntry> files;  // withFiles config deploys (profile, reversible)
};

struct Generation {
    int id = 0;
    int parent = 0;  // 0 = no parent
    std::string created_at;
    std::string set_hash;  // sha256 over the sorted derivation-hash set
    std::vector<std::string> env;  // PATH entries this generation wires
    std::vector<GenDeriv> derivations;
};

// Stable identity of the realized set: sha256 of the sorted node hashes.
// Two evaluations producing the same derivations have the same set_hash, which
// is how a re-apply is detected as a no-op.
std::string derivation_set_hash(const kuli::luau::DerivationGraph& g);

struct Profile {
    fs::path root;             // the profiles directory
    std::string name = "default";

    fs::path dir() const { return root / name; }

    int current_id() const;                       // 0 if no current generation
    std::optional<Generation> current() const;
    std::optional<Generation> load(int id) const;
    std::vector<int> list() const;                // ascending generation ids

    // Append a new generation built from the graph and point `current` at it.
    // (No-op detection is the caller's job — compare set_hash to current first.)
    Generation commit(const kuli::luau::DerivationGraph& g);

    // Point `current` at an existing generation (low-level; activate() is the
    // user-facing op).
    bool set_current(int id);

    // Re-project the target generation onto disk: remove shims the current
    // generation has but the target lacks, (re)write the target's shims to
    // point at its (immutable) store paths, then point `current` at it. This is
    // the rollback / switch mechanism (C3). Returns false if target is missing.
    bool activate(int target_id, const fs::path& bin_dir, const fs::path& store_root);
};

Profile default_profile();

}  // namespace kuli::bp

#pragma once
// The derivation graph a blueprint evaluates to — plain C++ structs (no Luau,
// no JSON in the interface). Produced by kuli-luau, consumed by kuli-bp /
// kuli-engine. A node's `hash` is its content-addressed identity (§8.2.1);
// children are referenced by hash so the graph is a DAG with no duplication.

#include <map>
#include <string>
#include <vector>

namespace kuli::luau {

enum class Builder { Fetch, Composite, WithFiles };

const char* builder_name(Builder b);

// One file deployed into the profile by withFiles (§8.2.6 — per-user config,
// not content-addressed into the store).
struct FileEntry {
    std::string path;
    std::string mode;  // "replace" | "patch"
    std::string content;
};

// fetchGitHubRelease specification (§8.2.5). The actual asset URL is resolved
// at realize time; the content is pinned by sha256, so the URL resolution does
// not affect the hash (keeps eval pure — R-NF-04).
struct FetchSpec {
    std::string owner;
    std::string repo;
    std::string version;
    std::string asset_pattern;
    std::string sha256;
    std::string bin;          // optional: relative path to the executable
    std::string shim_dir;     // optional: shim every exe under this dir
    std::string post_install; // optional: script run once on fresh extract
};

struct Derivation {
    Builder builder = Builder::Fetch;
    std::string hash;          // 64 hex (sha256 of the input closure)
    std::string name;
    std::string store_path;    // "<hash[:16]>-<name>"
    std::string system_target; // e.g. "windows-x64"

    FetchSpec fetch;                       // Builder::Fetch
    std::vector<std::string> components;   // Builder::Composite (ordered child hashes)
    std::vector<std::string> requires_;    // Builder::Composite
    std::string base;                      // Builder::WithFiles (child hash)
    std::vector<FileEntry> files;          // Builder::WithFiles
};

struct DerivationGraph {
    std::map<std::string, Derivation> nodes;  // keyed by hash
    std::string root;                          // the returned derivation's hash

    const Derivation* find(const std::string& h) const {
        auto it = nodes.find(h);
        return it == nodes.end() ? nullptr : &it->second;
    }
};

}  // namespace kuli::luau

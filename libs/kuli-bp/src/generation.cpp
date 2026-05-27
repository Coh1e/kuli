#include "kuli/bp/generation.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include <set>

#include "kuli/crypto/hash.hpp"
#include "kuli/platform/paths.hpp"
#include "kuli/platform/shim.hpp"

namespace kuli::bp {

namespace {
using nlohmann::json;
using kuli::luau::Builder;

std::string now_epoch() {
    auto t = std::chrono::system_clock::now();
    return std::to_string(
        std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count());
}

std::string shim_basename(const std::string& bin) {
    return fs::path(bin).stem().string();
}

// Expand a leading "~" to the home dir (config targets like "~/.config/...").
fs::path expand_path(const std::string& p) {
    if (p.empty() || p[0] != '~') return fs::path(p);
    std::string rest = (p.size() > 1 && (p[1] == '/' || p[1] == '\\')) ? p.substr(2) : p.substr(1);
    fs::path out = kuli::platform::paths::home();
    if (!rest.empty()) out /= fs::path(rest);
    return out;
}

// Deploy a config file, backing up a pre-existing (non-kuli) file once so
// rollback can restore it. v0 treats every mode as "replace" (overwrite);
// true patch-merge is deferred (v0.2+).
bool deploy_file(const fs::path& target, const std::string& content) {
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    fs::path bak = target;
    bak += ".kuli-bak";
    if (fs::exists(target, ec) && !fs::exists(bak, ec)) {
        fs::copy_file(target, bak, fs::copy_options::overwrite_existing, ec);
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(out);
}

// Undo a deployed file: restore the backup if we made one, else delete the file
// we created.
void remove_deployed(const fs::path& target) {
    std::error_code ec;
    fs::path bak = target;
    bak += ".kuli-bak";
    if (fs::exists(bak, ec)) {
        fs::remove(target, ec);
        fs::rename(bak, target, ec);
    } else {
        fs::remove(target, ec);
    }
}

json to_json(const Generation& g) {
    json derivs = json::array();
    for (const auto& d : g.derivations) {
        json shims = json::array();
        for (const auto& s : d.shims) shims.push_back({{"alias", s.alias}, {"bin", s.bin_rel}});
        json files = json::array();
        for (const auto& f : d.files) {
            files.push_back({{"path", f.path}, {"mode", f.mode}, {"content", f.content}});
        }
        json basenames = json::array();
        for (const auto& b : d.basenames) {
            basenames.push_back({{"alias", b.alias}, {"adapter", b.adapter}});
        }
        derivs.push_back({{"hash", d.hash},
                          {"name", d.name},
                          {"storePath", d.store_path},
                          {"shims", shims},
                          {"files", files},
                          {"basenames", basenames}});
    }
    return json{{"id", g.id},
                {"parent", g.parent},
                {"created_at", g.created_at},
                {"set_hash", g.set_hash},
                {"env", g.env},
                {"derivations", derivs}};
}

Generation from_json(const json& j) {
    Generation g;
    g.id = j.value("id", 0);
    g.parent = j.value("parent", 0);
    g.created_at = j.value("created_at", "");
    g.set_hash = j.value("set_hash", "");
    g.env = j.value("env", std::vector<std::string>{});
    for (const auto& d : j.value("derivations", json::array())) {
        GenDeriv e;
        e.hash = d.value("hash", "");
        e.name = d.value("name", "");
        e.store_path = d.value("storePath", "");
        for (const auto& s : d.value("shims", json::array())) {
            e.shims.push_back(Shim{s.value("alias", ""), s.value("bin", "")});
        }
        for (const auto& f : d.value("files", json::array())) {
            e.files.push_back(kuli::luau::FileEntry{f.value("path", ""), f.value("mode", "replace"),
                                                    f.value("content", "")});
        }
        for (const auto& b : d.value("basenames", json::array())) {
            e.basenames.push_back(Basename{b.value("alias", ""), b.value("adapter", "")});
        }
        g.derivations.push_back(std::move(e));
    }
    return g;
}

void write_json(const fs::path& p, const json& j) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (out) out << j.dump(2);
}

}  // namespace

std::string derivation_set_hash(const kuli::luau::DerivationGraph& g) {
    std::vector<std::string> hashes;
    hashes.reserve(g.nodes.size());
    for (const auto& [h, _] : g.nodes) hashes.push_back(h);
    std::sort(hashes.begin(), hashes.end());
    // Fold in the root so two identical node sets with different roots are
    // distinct generations (M-4); otherwise a re-apply could be a false no-op.
    std::string joined = "root:" + g.root + "\n";
    for (const auto& h : hashes) {
        joined += h;
        joined += '\n';
    }
    return kuli::crypto::sha256_hex(joined);
}

std::vector<int> Profile::list() const {
    std::vector<int> ids;
    std::error_code ec;
    fs::path gens = dir() / "generations";
    if (!fs::exists(gens, ec)) return ids;
    for (const auto& e : fs::directory_iterator(gens, ec)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".json") continue;
        try {
            ids.push_back(std::stoi(e.path().stem().string()));
        } catch (...) {
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

int Profile::current_id() const {
    std::ifstream in(dir() / "current", std::ios::binary);
    if (!in) return 0;
    int id = 0;
    in >> id;
    return in ? id : 0;
}

std::optional<Generation> Profile::load(int id) const {
    std::ifstream in(dir() / "generations" / (std::to_string(id) + ".json"), std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded()) return std::nullopt;
    return from_json(j);
}

std::optional<Generation> Profile::current() const {
    int id = current_id();
    if (id == 0) return std::nullopt;
    return load(id);
}

bool Profile::set_current(int id) {
    if (!load(id)) return false;
    std::error_code ec;
    fs::create_directories(dir(), ec);
    std::ofstream out(dir() / "current", std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << id;
    return static_cast<bool>(out);
}

Generation Profile::commit(const kuli::luau::DerivationGraph& g) {
    // Append-only: never reuse an id, even after a rollback moved `current` back.
    auto ids = list();
    int max_id = ids.empty() ? 0 : ids.back();

    Generation gen;
    gen.id = max_id + 1;
    gen.parent = current_id();
    gen.created_at = now_epoch();
    gen.set_hash = derivation_set_hash(g);
    gen.env = {"~/.local/bin"};
    for (const auto& [h, d] : g.nodes) {
        GenDeriv e{d.hash, d.name, d.store_path, {}, {}, {}};
        if (d.builder == Builder::Fetch && !d.fetch.bin.empty()) {
            e.shims.push_back(Shim{shim_basename(d.fetch.bin), d.fetch.bin});
        }
        if (d.builder == Builder::WithFiles) {
            e.files = d.files;  // config files this derivation deploys to the profile
        }
        if (d.builder == Builder::Scripture) {
            for (const auto& [alias, adapter] : d.scripture.basenames) {
                e.basenames.push_back(Basename{alias, adapter});
            }
        }
        gen.derivations.push_back(std::move(e));
    }

    // Append only; activate() is what moves `current` (so rollback can still
    // see the previous generation's shims to reconcile against).
    write_json(dir() / "generations" / (std::to_string(gen.id) + ".json"), to_json(gen));
    return gen;
}

bool Profile::activate(int target_id, const fs::path& bin_dir, const fs::path& store_root) {
    auto target = load(target_id);
    if (!target) return false;

    auto cur = current();

    std::set<std::string> target_aliases;
    std::set<std::string> target_files;  // expanded paths
    for (const auto& d : target->derivations) {
        for (const auto& s : d.shims) target_aliases.insert(s.alias);
        for (const auto& f : d.files) target_files.insert(expand_path(f.path).string());
        for (const auto& b : d.basenames) target_aliases.insert(b.alias);  // share ~/.local/bin
    }

    // Remove shims + basenames + config files the current generation projected
    // that the target does not (config files restore their backup).
    if (cur) {
        for (const auto& d : cur->derivations) {
            for (const auto& s : d.shims) {
                if (!target_aliases.count(s.alias)) kuli::platform::remove_shim(bin_dir, s.alias);
            }
            for (const auto& b : d.basenames) {
                if (!target_aliases.count(b.alias)) kuli::platform::remove_shim(bin_dir, b.alias);
            }
            for (const auto& f : d.files) {
                fs::path ep = expand_path(f.path);
                if (!target_files.count(ep.string())) remove_deployed(ep);
            }
        }
    }

    // (Re)write the target's shims (point at its immutable store paths), config
    // files, and scripture basename shims (re-enter kuli via --basename).
    bool ok = true;
    fs::path kuli_exe = kuli::platform::paths::current_exe();
    for (const auto& d : target->derivations) {
        for (const auto& s : d.shims) {
            fs::path tgt = store_root / d.store_path / fs::path(s.bin_rel);
            if (!kuli::platform::write_shim(bin_dir, s.alias, tgt)) ok = false;
        }
        for (const auto& f : d.files) {
            if (!deploy_file(expand_path(f.path), f.content)) ok = false;
        }
        for (const auto& b : d.basenames) {
            if (!kuli::platform::write_basename_shim(bin_dir, b.alias, kuli_exe)) ok = false;
        }
    }
    // Don't advance `current` on a partial projection — leaving it at the old
    // generation lets a re-apply detect the diff and repair (M-2).
    if (!ok) return false;
    return set_current(target_id);
}

Profile default_profile() {
    return Profile{kuli::platform::paths::profiles_dir(), "default"};
}

}  // namespace kuli::bp

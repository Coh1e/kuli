#pragma once
// Capability / mesh layer (§6). A node publishes a CapabilityRecord (what it is
// + can do); peers' records are cached; a matcher evaluates constraints against
// records; and `route` picks a capable peer and dispatches the job over the
// transport. v0 is unsigned + statically discovered (sync pulls a peer's record
// through the transport); signing + gossip + monotonic versioning come later.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kuli::bp {

namespace fs = std::filesystem;

// This host's CapabilityRecord: { host, os, arch, cpu, kuli, scriptures[], v }.
nlohmann::json local_capability();

// True if `record` satisfies every constraint. Constraints are "key op value":
//   os=/arch=/host=/kuli=  (string equality)
//   cpu>=/cpu>/cpu<=/cpu</cpu=  (numeric)
//   scripture=<name>  (record.scriptures contains <name>)
bool capability_matches(const nlohmann::json& record, const std::vector<std::string>& constraints);

// `kuli capability [--json]` — show this host's record.
int capability_show(bool json);
// `kuli capability sync <alias>` — pull a peer's record (over the transport) and cache it.
int capability_sync(const std::string& alias);
// `kuli capability ls` — list the local + cached records.
int capability_list();

// `kuli route --need <c>... [--fallback fail|local] -- <cmd...>` — run <cmd> on
// the first peer (local or cached) satisfying the constraints.
int route(const std::vector<std::string>& need, const std::string& fallback,
          const std::vector<std::string>& cmd, const fs::path& cwd);

}  // namespace kuli::bp

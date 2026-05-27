#pragma once
// Host aliases for the transport layer: `@<alias>` in an IR node's `at:` resolves
// through this registry (~/.config/kuli/hosts.json) to a concrete transport
// (ssh:<target> or local-subprocess:). The engine reads the registry; these
// helpers back the `kuli host` CLI that writes it.

#include <string>

namespace kuli::bp {

// `kuli host add <alias> [target] [--transport ssh|local-subprocess]`.
int host_add(const std::string& alias, const std::string& target, const std::string& transport);
int host_remove(const std::string& alias);
int host_list();

}  // namespace kuli::bp

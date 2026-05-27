#pragma once
// Host aliases for the transport layer: `@<alias>` in an IR node's `at:` resolves
// through this registry (~/.config/kuli/hosts.json) to a concrete transport
// (ssh:<target> or local-subprocess:). The engine reads the registry; these
// helpers back the `kuli host` CLI that writes it.

#include <string>

namespace kuli::bp {

// A registered transport target. Only `transport` + `target` are required;
// the rest are optional ssh connection options.
struct HostSpec {
    std::string target;             // user@host (ssh); ignored for local-subprocess
    std::string transport = "ssh";  // "ssh" | "local-subprocess"
    std::string port;               // ssh -p
    std::string identity;           // ssh -i <keyfile>
    std::string extra;              // extra ssh args (whitespace-split)
    std::string remote_kuli;        // kuli path on the remote (default "kuli")
};

// `kuli host add <alias> [target] [--transport ...] [--port ...] [--identity ...]
//  [--extra ...] [--remote-kuli ...]`.
int host_add(const std::string& alias, const HostSpec& spec);
int host_remove(const std::string& alias);
int host_list();

}  // namespace kuli::bp

#pragma once
// Blocking, cross-platform process launch — the simple wrapper that eliminates
// the CreateProcessW-vs-fork/exec difference (the user's lib policy: a simple
// native wrapper for blocking exec; libuv is reserved for the async case).
// stdio is inherited (child output streams straight to the terminal); only the
// exit code is returned. Capture / async are later refinements.

#include <filesystem>
#include <string>
#include <vector>

namespace kuli::platform {

namespace fs = std::filesystem;

struct ProcessResult {
    bool launched = false;  // the process was started (exec may still have failed on POSIX)
    int exit_code = -1;     // process exit code (128+signal if terminated by a signal)
};

// Run argv[0] (PATH-searched) with argv[1..], blocking until it exits. `cwd`
// (if non-empty) is the child's working directory. argv must be non-empty.
ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd = {});

}  // namespace kuli::platform

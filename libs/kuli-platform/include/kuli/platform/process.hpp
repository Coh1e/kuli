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
    std::string output;     // captured stdout+stderr (only when capture = true)
};

// Run argv[0] (PATH-searched) with argv[1..], blocking until it exits. `cwd`
// (if non-empty) is the child's working directory. argv must be non-empty.
// When `capture` is true the child's stdout+stderr are redirected to a temp
// file and returned in `output` (deadlock-free, no pipe draining); otherwise
// stdio is inherited and streams straight to the terminal. When `input` is
// non-empty it is fed to the child's stdin (via a temp file) — e.g. piping an
// IR document to `kuli run-ir -`.
ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd = {},
                          bool capture = false, const std::string& input = {});

}  // namespace kuli::platform

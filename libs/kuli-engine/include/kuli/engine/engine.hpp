#pragma once
// kuli-engine — the C++ ABI boundary (design §2.4). Front-ends (CLI / meshell /
// remote agent) link ONLY this; they never see kuli-ir / kuli-store internals.
// execute() validates the IR, opens an evidence session, expands the L1 intent
// into an L2 plan, and either previews (--dry-run) or runs it.

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace kuli::engine {

namespace fs = std::filesystem;

// Where the executor realizes content. The executor only touches the store;
// shim/PATH projection is the profile-activation layer's job (kuli-bp), keeping
// realize (content) cleanly separate from activation (the per-profile view).
// Tests inject temp roots so a unit test never touches the user's store.
struct ExecEnv {
    fs::path store_root;
    fs::path downloads;
};

// The real XDG store + download cache.
ExecEnv default_exec_env();

struct AdapterCall {
    std::string tool_name;
    std::vector<std::string> argv;
    fs::path cwd;
    nlohmann::json ir_doc;  // kuli/ir/1.0; envelope.options.dry_run gates writes
};

struct RawResult {
    int exit_code = 0;
    std::vector<std::string> lines;            // human stdout
    std::string raw_stderr;                    // rendered diagnostics
    std::optional<nlohmann::json> structured;  // --json payload
    std::string session_id;                    // evidence session id (kuli extension)
};

class Engine {
   public:
    enum class Mode { Run };
    explicit Engine(Mode mode = Mode::Run);
    explicit Engine(ExecEnv env, Mode mode = Mode::Run) : env_(std::move(env)), mode_(mode) {}

    // Dispatch on ir_doc.kind. Writes an evidence session under
    // cwd/.kuli/sessions/<id>/ on every Run. On --dry-run, stops after the
    // plan is written (nothing else touches disk / HKCU).
    RawResult execute(const AdapterCall& call);

   private:
    ExecEnv env_;
    Mode mode_;
};

}  // namespace kuli::engine

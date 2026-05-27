#include "kuli/bp/meshell.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kuli/engine/engine.hpp"
#include "kuli/ir/ir.hpp"

namespace kuli::bp {

namespace {
using nlohmann::json;

kuli::diag::Diagnostic err(const std::string& msg) {
    return kuli::diag::Diagnostic::error(msg, "E0660");
}

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> ws_split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> pipe_split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '|') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// "@local" -> "local:"; "@<alias>" -> kept as-is (engine resolves it); other -> as-is.
std::string target_to_at(const std::string& target) {
    if (target == "@local") return "local:";
    return target;  // "@prod" stays an alias; engine route_remote resolves it
}

struct Stage {
    std::string at = "local:";
    std::vector<std::string> cmd;
};

std::expected<Stage, kuli::diag::Diagnostic> parse_stage(const std::string& seg) {
    std::vector<std::string> tokens = ws_split(trim(seg));
    if (tokens.empty()) return std::unexpected(err("empty pipeline stage"));

    Stage st;
    std::size_t cmd_from = 0;
    if (!tokens[0].empty() && tokens[0][0] == '@') {
        const std::string& head = tokens[0];
        auto colon = head.find(':');
        if (colon != std::string::npos) {  // "@a:cat" — target then inline command head
            st.at = target_to_at(head.substr(0, colon));
            std::string rest = head.substr(colon + 1);
            if (!rest.empty()) st.cmd.push_back(rest);
        } else {  // "@a cat" — target is its own token
            st.at = target_to_at(head);
        }
        cmd_from = 1;
    }
    for (std::size_t i = cmd_from; i < tokens.size(); ++i) st.cmd.push_back(tokens[i]);

    if (st.cmd.empty()) {
        return std::unexpected(err("stage '" + tokens[0] + "' has a target but no command"));
    }
    return st;
}

}  // namespace

std::expected<json, kuli::diag::Diagnostic> parse_meshell(const std::string& line) {
    if (trim(line).empty()) return std::unexpected(err("empty meshell command"));

    std::vector<Stage> stages;
    for (const auto& seg : pipe_split(line)) {
        auto st = parse_stage(seg);
        if (!st) return std::unexpected(st.error());
        stages.push_back(std::move(*st));
    }

    json ir;
    ir["schema"] = std::string(kuli::ir::SCHEMA);
    if (stages.size() == 1) {
        ir["kind"] = std::string(kuli::ir::kind::Exec);
        ir["node"] = {{"at", stages[0].at}, {"cmd", stages[0].cmd}};
    } else {
        ir["kind"] = std::string(kuli::ir::kind::Pipeline);
        json arr = json::array();
        for (const auto& s : stages) arr.push_back({{"at", s.at}, {"cmd", s.cmd}});
        ir["node"] = {{"stages", arr}};
    }
    return ir;
}

int run_mesh(const std::string& line, const fs::path& cwd) {
    auto ir = parse_meshell(line);
    if (!ir) {
        std::cerr << kuli::diag::render(ir.error(), /*color=*/false);
        return kuli::diag::exit_code_of(ir.error());
    }
    kuli::engine::Engine engine;
    kuli::engine::AdapterCall call;
    call.tool_name = "mesh";
    call.cwd = cwd;
    call.ir_doc = *ir;
    kuli::engine::RawResult rr = engine.execute(call);
    for (const auto& l : rr.lines) std::cout << l << "\n";
    if (!rr.raw_stderr.empty()) std::cerr << rr.raw_stderr;
    return rr.exit_code;
}

}  // namespace kuli::bp

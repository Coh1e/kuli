#include "kuli/platform/process.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace kuli::platform {

namespace {

// A unique temp path for staging a child's stdin / capturing its stdout+stderr
// (a file avoids the pipe-buffer deadlock you'd get without a drain thread).
fs::path scratch_temp(const char* tag) {
    static std::atomic<unsigned> ctr{0};
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "kuli-" << tag << "-" << t << "-" << ctr.fetch_add(1) << ".tmp";
    return fs::temp_directory_path() / name.str();
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void remove_quiet(const fs::path& p) {
    if (p.empty()) return;
    std::error_code ec;
    fs::remove(p, ec);
}

#if defined(_WIN32)
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quote an argument per the CommandLineToArgvW rules so the child re-parses argv
// exactly as supplied (the classic backslash-before-quote handling).
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
        } else {
            if (backslashes) {
                out.append(backslashes, L'\\');
                backslashes = 0;
            }
            out += c;
        }
    }
    out.append(backslashes * 2, L'\\');  // trailing backslashes before the closing quote
    out += L'"';
    return out;
}
#endif

}  // namespace

ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd, bool capture,
                          const std::string& input) {
    ProcessResult r;
    if (argv.empty()) return r;

    fs::path out_tmp = capture ? scratch_temp("out") : fs::path{};
    fs::path in_tmp;
    if (!input.empty()) {
        in_tmp = scratch_temp("in");
        std::ofstream o(in_tmp, std::ios::binary | std::ios::trunc);
        o << input;
    }
    bool use_handles = capture || !input.empty();

#if defined(_WIN32)
    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += L' ';
        cmdline += quote_arg(widen(argv[i]));
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');
    std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    HANDLE hin = INVALID_HANDLE_VALUE, hout = INVALID_HANDLE_VALUE;
    BOOL inherit = FALSE;
    if (use_handles) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        si.dwFlags |= STARTF_USESTDHANDLES;
        if (!input.empty()) {
            hin = CreateFileW(in_tmp.wstring().c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
            si.hStdInput = hin;
        } else {
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
        if (capture) {
            hout = CreateFileW(out_tmp.wstring().c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
            si.hStdOutput = hout;
            si.hStdError = hout;
        } else {
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        }
        inherit = TRUE;
    }
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, inherit, 0, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (hin != INVALID_HANDLE_VALUE) CloseHandle(hin);
    if (hout != INVALID_HANDLE_VALUE) CloseHandle(hout);
    if (!ok) {
        remove_quiet(in_tmp);
        remove_quiet(out_tmp);
        return r;  // launched = false
    }
    r.launched = true;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    r.exit_code = static_cast<int>(code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    pid_t pid = fork();
    if (pid < 0) {
        remove_quiet(in_tmp);
        remove_quiet(out_tmp);
        return r;
    }
    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        if (!input.empty()) {
            int fd = open(in_tmp.c_str(), O_RDONLY);
            if (fd >= 0) {
                dup2(fd, 0);
                close(fd);
            }
        }
        if (capture) {
            int fd = open(out_tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0) _exit(127);
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);  // exec failed
    }
    r.launched = true;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        r.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.exit_code = 128 + WTERMSIG(status);
    }
    (void)use_handles;
#endif

    if (capture) r.output = slurp(out_tmp);
    remove_quiet(in_tmp);
    remove_quiet(out_tmp);
    return r;
}

}  // namespace kuli::platform

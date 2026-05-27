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

// A unique temp path for capturing a child's stdout+stderr (a file avoids the
// pipe-buffer deadlock you'd get without a concurrent drain thread).
fs::path capture_temp() {
    static std::atomic<unsigned> ctr{0};
    auto t = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << "kuli-exec-" << t << "-" << ctr.fetch_add(1) << ".out";
    return fs::temp_directory_path() / name.str();
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd, bool capture) {
    ProcessResult r;
    if (argv.empty()) return r;
    fs::path tmp = capture ? capture_temp() : fs::path{};

#if defined(_WIN32)
    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += L' ';
        cmdline += quote_arg(widen(argv[i]));
    }
    std::vector<wchar_t> mutable_cmd(cmdline.begin(), cmdline.end());
    mutable_cmd.push_back(L'\0');  // CreateProcessW needs a writable buffer
    std::wstring wcwd = cwd.empty() ? std::wstring() : cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    HANDLE hout = INVALID_HANDLE_VALUE;
    BOOL inherit = FALSE;
    if (capture) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        hout = CreateFileW(tmp.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (hout == INVALID_HANDLE_VALUE) return r;
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hout;
        si.hStdError = hout;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        inherit = TRUE;
    }
    // No creation flags: a console child shares our console when not capturing.
    BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, inherit, 0, nullptr,
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi);
    if (hout != INVALID_HANDLE_VALUE) CloseHandle(hout);  // child holds its own copy
    if (!ok) {
        if (capture) {
            std::error_code ec;
            fs::remove(tmp, ec);
        }
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
    if (pid < 0) return r;  // launched = false
    if (pid == 0) {
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        if (capture) {
            int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
        _exit(127);  // exec failed (command not found)
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
#endif

    if (capture) {
        r.output = slurp(tmp);
        std::error_code ec;
        fs::remove(tmp, ec);
    }
    return r;
}

}  // namespace kuli::platform

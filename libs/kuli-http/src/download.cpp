#include "kuli/http/download.hpp"

#include <curl/curl.h>

#include <fstream>
#include <mutex>
#include <ostream>

#include "kuli/platform/env.hpp"

namespace kuli::http {

namespace {

using kuli::diag::Diagnostic;
using kuli::diag::Kind;

void ensure_global_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

std::size_t write_to_ostream(char* ptr, std::size_t size, std::size_t nmemb, void* ud) {
    auto* os = static_cast<std::ostream*>(ud);
    std::size_t n = size * nmemb;
    os->write(ptr, static_cast<std::streamsize>(n));
    return os->good() ? n : 0;
}

int xferinfo(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    auto* fn = static_cast<ProgressFn*>(ud);
    if (fn && *fn) (*fn)(static_cast<std::uint64_t>(dlnow), static_cast<std::uint64_t>(dltotal));
    return 0;
}

// Common easy-handle setup shared by file + text fetches.
void configure(CURL* h, const std::string& url) {
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "kuli/0.1");
    curl_easy_setopt(h, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip/deflate transfer
    // We build libcurl against OpenSSL (design §12.1, cross-platform) which has
    // no CA bundle on Windows by default. Use the OS trust store instead of
    // shipping a ca-bundle.crt; harmless where unsupported.
    curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
}

Diagnostic curl_error(const std::string& url, CURLcode rc, CURL* h) {
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    std::string msg = std::string("download failed: ") + curl_easy_strerror(rc);
    if (status >= 400) msg += " (HTTP " + std::to_string(status) + ")";
    return Diagnostic::of(Kind::General, msg + " — " + url, "E0200");
}

}  // namespace

std::string apply_mirror(std::string url) {
    auto prefix = kuli::platform::get_env("KULI_GITHUB_MIRROR_PREFIX");
    if (!prefix || prefix->empty()) return url;
    if (url.find("api.github.com") != std::string::npos) return url;  // never mirror the API
    bool is_gh = url.rfind("https://github.com", 0) == 0 ||
                 url.find("githubusercontent.com") != std::string::npos;
    if (!is_gh) return url;
    std::string p = *prefix;
    if (!p.empty() && p.back() == '/') p.pop_back();
    return p + "/" + url;
}

std::expected<void, Diagnostic> fetch_to_file(std::string_view url, const fs::path& dest,
                                              const FetchOptions& opts) {
    ensure_global_init();
    std::string final_url = apply_mirror(std::string(url));

    CURL* h = curl_easy_init();
    if (!h) return std::unexpected(Diagnostic::of(Kind::Internal, "curl init failed", "E7200"));

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        curl_easy_cleanup(h);
        return std::unexpected(
            Diagnostic::error("cannot open download target: " + dest.string(), "E0201"));
    }

    configure(h, final_url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_to_ostream);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);

    ProgressFn prog = opts.progress;
    if (prog) {
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xferinfo);
        curl_easy_setopt(h, CURLOPT_XFERINFODATA, &prog);
    } else {
        curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
    }

    CURLcode rc = curl_easy_perform(h);
    out.flush();
    out.close();

    if (rc != CURLE_OK) {
        Diagnostic d = curl_error(final_url, rc, h);
        curl_easy_cleanup(h);
        std::error_code ec;
        fs::remove(dest, ec);
        return std::unexpected(std::move(d));
    }
    curl_easy_cleanup(h);

    if (opts.expected) {
        if (!kuli::crypto::verify_file(dest, *opts.expected)) {
            auto actual = kuli::crypto::hash_file(dest, opts.expected->algo);
            std::error_code ec;
            fs::remove(dest, ec);
            return std::unexpected(
                Diagnostic::error("sha256 mismatch for " + std::string(url), "E0202")
                    .with_help("expected " + opts.expected->hex + ", got " +
                               (actual ? actual->hex : std::string("<unreadable>"))));
        }
    }
    return {};
}

std::expected<std::string, Diagnostic> fetch_text(std::string_view url) {
    ensure_global_init();
    std::string final_url = apply_mirror(std::string(url));

    CURL* h = curl_easy_init();
    if (!h) return std::unexpected(Diagnostic::of(Kind::Internal, "curl init failed", "E7200"));

    std::string body;
    auto write_str = [](char* ptr, std::size_t size, std::size_t nmemb, void* ud) -> std::size_t {
        static_cast<std::string*>(ud)->append(ptr, size * nmemb);
        return size * nmemb;
    };
    configure(h, final_url);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION,
                     static_cast<std::size_t (*)(char*, std::size_t, std::size_t, void*)>(write_str));
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        Diagnostic d = curl_error(final_url, rc, h);
        curl_easy_cleanup(h);
        return std::unexpected(std::move(d));
    }
    curl_easy_cleanup(h);
    return body;
}

}  // namespace kuli::http

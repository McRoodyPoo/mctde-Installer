#include "Download.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#include <filesystem>
#include <mutex>
#endif

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace mctde {

#ifdef _WIN32

static std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

// Core: GET a URL, follow redirects, stream the body to outPath. Reports the
// HTTP status and whether the response was text/html (used to spot Drive's
// confirm/quota interstitial vs the actual file).
static bool httpGetToFileW(const std::wstring& url, const std::string& outPath,
                           const ProgressFn& progress,
                           long& statusOut, bool& isHtmlOut, std::string& err) {
    statusOut = 0;
    isHtmlOut = false;

    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[8192] = {0}, extra[4096] = {0};
    uc.lpszHostName = host;     uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;      uc.dwUrlPathLength = 8192;
    uc.lpszExtraInfo = extra;   uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }

    bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring fullPath = std::wstring(path) + std::wstring(extra);

    HINTERNET hS = WinHttpOpen(L"mctde-installer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) { err = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(hS, 8000, 8000, 30000, 30000);

    bool ok = false;
    HINTERNET hC = WinHttpConnect(hS, host, uc.nPort, 0);
    if (hC) {
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET", fullPath.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          secure ? WINHTTP_FLAG_SECURE : 0);
        if (hR) {
            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hR, nullptr)) {

                DWORD status = 0, sz = sizeof(status);
                WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                statusOut = status;

                wchar_t ctype[256] = {0}; DWORD ctlen = sizeof(ctype);
                if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                        ctype, &ctlen, WINHTTP_NO_HEADER_INDEX))
                    isHtmlOut = (wcsstr(ctype, L"text/html") != nullptr);

                uint64_t total = 0;
                wchar_t clen[64] = {0}; DWORD cllen = sizeof(clen);
                if (WinHttpQueryHeaders(hR, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        clen, &cllen, WINHTTP_NO_HEADER_INDEX))
                    total = _wcstoui64(clen, nullptr, 10);

                // outPath is UTF-8; u8path opens it with the wide API so a
                // non-ASCII user folder works (a plain narrow path would be
                // read in the ANSI code page).
                std::ofstream o(std::filesystem::u8path(outPath), std::ios::binary | std::ios::trunc);
                if (o) {
                    uint64_t got = 0;
                    bool cancelled = false;
                    DWORD avail = 0;
                    do {
                        if (!WinHttpQueryDataAvailable(hR, &avail)) break;
                        if (!avail) break;
                        std::vector<char> buf(avail);
                        DWORD read = 0;
                        if (!WinHttpReadData(hR, buf.data(), avail, &read) || read == 0) break;
                        o.write(buf.data(), read);
                        got += read;
                        if (progress && !progress(got, total)) { cancelled = true; break; }
                    } while (avail > 0);
                    o.close();
                    ok = (status == 200) && (got > 0) && !cancelled;
                    if (cancelled) err = "cancelled";
                } else {
                    err = "cannot write " + outPath;
                }
            } else {
                err = "request failed";
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    } else {
        err = "connect failed";
    }
    WinHttpCloseHandle(hS);
    return ok;
}

static bool httpGetToFile(const std::string& url, const std::string& outPath,
                          const ProgressFn& progress,
                          long& statusOut, bool& isHtmlOut, std::string& err) {
    return httpGetToFileW(widen(url), outPath, progress, statusOut, isHtmlOut, err);
}

#else  // ---- native Linux: libcurl ----

// libcurl's own CA path is fixed at build time (e.g. Arch's), which may not
// exist on the distro running the AppImage, so pick the first bundle present.
static const char* caBundle() {
    static const char* const candidates[] = {
        "/etc/ssl/certs/ca-certificates.crt",   // Arch, Debian/Ubuntu, SteamOS
        "/etc/pki/tls/certs/ca-bundle.crt",     // Fedora/RHEL
        "/etc/ssl/ca-bundle.pem",               // openSUSE
        "/etc/ssl/cert.pem",                    // Alpine, others
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
    };
    for (const char* c : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c;
    }
    return nullptr;   // fall back to libcurl's compiled-in default
}

struct CurlSink {
    std::FILE* f = nullptr;
    const ProgressFn* progress = nullptr;
    bool cancelled = false;
};

static size_t curlWrite(char* ptr, size_t size, size_t nmemb, void* user) {
    CurlSink* s = static_cast<CurlSink*>(user);
    return std::fwrite(ptr, size, nmemb, s->f) * size;
}

static int curlProgress(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    CurlSink* s = static_cast<CurlSink*>(user);
    if (s->progress && *s->progress && dlnow > 0 &&
        !(*s->progress)((uint64_t)dlnow, (uint64_t)(dltotal > 0 ? dltotal : 0))) {
        s->cancelled = true;
        return 1;   // abort the transfer
    }
    return 0;
}

static bool httpGetToFile(const std::string& url, const std::string& outPath,
                          const ProgressFn& progress,
                          long& statusOut, bool& isHtmlOut, std::string& err) {
    static std::once_flag init;
    std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    statusOut = 0;
    isHtmlOut = false;

    CurlSink sink;
    sink.f = std::fopen(outPath.c_str(), "wb");
    if (!sink.f) { err = "cannot write " + outPath; return false; }
    sink.progress = &progress;

    CURL* c = curl_easy_init();
    if (!c) { std::fclose(sink.f); err = "curl init failed"; return false; }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "mctde-installer/1.0");
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);    // give up if stalled
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);    // for 30 s
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, curlProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    if (const char* ca = caBundle()) curl_easy_setopt(c, CURLOPT_CAINFO, ca);

    CURLcode rc = curl_easy_perform(c);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    char* ctype = nullptr;
    curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ctype);
    if (ctype && std::strstr(ctype, "text/html")) isHtmlOut = true;
    curl_off_t got = 0;
    curl_easy_getinfo(c, CURLINFO_SIZE_DOWNLOAD_T, &got);
    curl_easy_cleanup(c);
    std::fclose(sink.f);

    statusOut = status;
    if (sink.cancelled) { err = "cancelled"; return false; }
    if (rc != CURLE_OK) { err = curl_easy_strerror(rc); return false; }
    if (status != 200) { err = "HTTP " + std::to_string(status); return false; }
    return got > 0;
}

#endif

bool downloadUrl(const std::string& url, const std::string& outPath,
                 std::string& err, const ProgressFn& progress) {
    long status = 0; bool html = false;
    return httpGetToFile(url, outPath, progress, status, html, err);
}

// Pull the value of an HTML hidden-input by name from the Drive confirm form.
static std::string formValue(const std::string& html, const std::string& name) {
    std::string needle = "name=\"" + name + "\"";
    size_t p = html.find(needle);
    if (p == std::string::npos) return "";
    size_t v = html.find("value=\"", p);
    if (v == std::string::npos) return "";
    v += 7;
    size_t e = html.find('"', v);
    if (e == std::string::npos) return "";
    return html.substr(v, e - v);
}

bool downloadGoogleDrive(const std::string& fileId, const std::string& outPath,
                         std::string& err, const ProgressFn& progress) {
    const std::string base = "https://drive.usercontent.google.com/download?id=" + fileId +
                             "&export=download";
    long status = 0; bool html = false;

    // First try with confirm=t, which serves most large files directly.
    if (!httpGetToFile(base + "&confirm=t", outPath, progress, status, html, err)) {
        if (err == "cancelled") return false;  // test-mode early cancel
        // fall through to parse-the-form path below if we at least got a page
    }
    if (!html) return status == 200;

    // We got the confirm/quota interstitial. Parse it and re-request.
    std::ifstream f(outPath, std::ios::binary);
    std::string page((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::string confirm = formValue(page, "confirm");
    std::string uuid = formValue(page, "uuid");
    if (confirm.empty() && uuid.empty()) {
        err = "Google Drive did not serve the file (quota exceeded or link changed)";
        return false;
    }
    std::string url2 = base;
    if (!confirm.empty()) url2 += "&confirm=" + confirm;
    if (!uuid.empty())    url2 += "&uuid=" + uuid;

    if (!httpGetToFile(url2, outPath, progress, status, html, err)) return false;
    if (html) { err = "Google Drive still returned a page (quota?)"; return false; }
    return status == 200;
}

} // namespace mctde

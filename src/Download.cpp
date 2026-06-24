#include "Download.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <fstream>
#include <vector>

namespace mctde {

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
static bool httpGetToFile(const std::wstring& url, const std::string& outPath,
                          const ProgressFn& progress,
                          DWORD& statusOut, bool& isHtmlOut, std::string& err) {
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

                std::ofstream o(outPath, std::ios::binary | std::ios::trunc);
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

bool downloadUrl(const std::string& url, const std::string& outPath,
                 std::string& err, const ProgressFn& progress) {
    DWORD status = 0; bool html = false;
    return httpGetToFile(widen(url), outPath, progress, status, html, err);
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
    DWORD status = 0; bool html = false;

    // First try with confirm=t, which serves most large files directly.
    if (!httpGetToFile(widen(base + "&confirm=t"), outPath, progress, status, html, err)) {
        if (err == "cancelled") return false;  // test-mode early cancel
        // fall through to parse-the-form path below if we at least got a page
    }
    if (!html) return status == 200;

    // We got the confirm/quota interstitial — parse it and re-request.
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

    if (!httpGetToFile(widen(url2), outPath, progress, status, html, err)) return false;
    if (html) { err = "Google Drive still returned a page (quota?)"; return false; }
    return status == 200;
}

} // namespace mctde

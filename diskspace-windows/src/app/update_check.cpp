#include "update_check.h"

#include <windows.h>

#include <winhttp.h>

#include <cmath>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../../core/json.h"
#include "../core/win_util.h"
#include "preferences.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "version.lib")

namespace app {
namespace {

constexpr const wchar_t* kHost = L"diskspace.io";
constexpr const wchar_t* kPath = L"/api/latest-version";
/// What this build reports itself as. The site answers for every platform it
/// knows, and treats one it does not as "nothing available" rather than an error.
constexpr const wchar_t* kRuntime = L"windows";

// ---- The request ---------------------------------------------------------

/// Fetches the endpoint. Empty on any failure: a check that could not reach the
/// site is not something to bother the user with.
std::string fetch(const std::wstring& query) {
    // Overridable so a development build can be pointed at a local copy of the
    // site, the same way the theme is.
    std::wstring host = kHost;
    std::wstring path = kPath;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
    wchar_t override[512] = {};
    if (GetEnvironmentVariableW(L"DISKSPACE_UPDATE_URL", override, 512) > 0) {
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t hostName[256] = {}, urlPath[512] = {};
        parts.lpszHostName = hostName;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = urlPath;
        parts.dwUrlPathLength = 512;
        if (WinHttpCrackUrl(override, 0, 0, &parts)) {
            host = hostName;
            path = urlPath;
            port = parts.nPort;
            secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
        }
    }

    std::string body;
    HINTERNET session = WinHttpOpen(L"DiskSpace", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    // Eight seconds all in. A launch must never wait on this.
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    HINTERNET connection = WinHttpConnect(session, host.c_str(), port, 0);
    if (connection) {
        std::wstring target = path + L"?" + query;
        HINTERNET request =
            WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
                               WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (request) {
            // The answer changes only when a release is cut, but a stale cache
            // would hide exactly the case this exists for.
            WinHttpAddRequestHeaders(request, L"Cache-Control: no-cache", DWORD(-1),
                                     WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                                   0, 0, 0) &&
                WinHttpReceiveResponse(request, nullptr)) {
                DWORD status = 0, size = sizeof(status);
                WinHttpQueryHeaders(request,
                                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                                    &status, &size, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    DWORD available = 0;
                    char buffer[4096];
                    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
                        DWORD read = 0;
                        DWORD take = available < sizeof(buffer) ? available : DWORD(sizeof(buffer));
                        if (!WinHttpReadData(request, buffer, take, &read) || read == 0) break;
                        body.append(buffer, read);
                        // A runaway response is a bug at the other end, not
                        // something to keep reading.
                        if (body.size() > (1 << 20)) break;
                    }
                }
            }
            WinHttpCloseHandle(request);
        }
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return body;
}

/// Days between a yyyy-mm-dd calendar date, read as UTC, and now.
int daysSince(const std::wstring& date) {
    return diskscan::daysSince(diskscan::toUtf8(date));
}

}  // namespace

std::wstring UpdateAvailable::summary() const {
    std::wstring line = L"Version " + version + L" is available.";
    int days = daysSince(currentReleaseDate);
    if (days >= 1) {
        std::wstring age;
        if (days < 45) {
            age = days == 1 ? L"a day" : std::to_wstring(days) + L" days";
        } else {
            int months = int(std::lround(double(days) / 30.44));
            if (months < 18) {
                age = months == 1 ? L"a month" : std::to_wstring(months) + L" months";
            } else {
                double years = double(days) / 365.25;
                age = years < 1.5 ? L"a year" : std::to_wstring(int(std::lround(years))) + L" years";
            }
        }
        line += L" Yours is " + age + L" old.";
    }
    return line;
}

UpdateCheck& UpdateCheck::shared() {
    static UpdateCheck instance;
    return instance;
}

std::wstring UpdateCheck::currentVersion() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0) return L"0.0.0";
    std::vector<uint8_t> block(size);
    if (!GetFileVersionInfoW(path, 0, size, block.data())) return L"0.0.0";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(block.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return L"0.0.0";
    }
    // The build field is deliberately left out: the site's versions are three
    // components, and sending a fourth would never match a published release.
    return std::to_wstring(HIWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(LOWORD(info->dwFileVersionMS)) + L"." +
           std::to_wstring(HIWORD(info->dwFileVersionLS));
}

bool UpdateCheck::isNewer(const std::wstring& candidate, const std::wstring& current) {
    return diskscan::isNewerVersion(diskscan::toUtf8(candidate), diskscan::toUtf8(current));
}

void UpdateCheck::check(HWND notify) {
    if (started_) {
        // Already answered, or still in flight. Either way the caller gets the
        // same answer the first one will.
        if (finished_.load(std::memory_order_acquire) && found_ && notify) {
            PostMessageW(notify, kMessage, 0, 0);
        }
        return;
    }
    started_ = true;
    std::thread([this, notify] { run(notify); }).detach();
}

void UpdateCheck::run(HWND notify) {
    const std::wstring current = currentVersion();
    std::wstring query = std::wstring(L"platform=") + kRuntime + L"&version=" + current;
    std::string body = fetch(query);

    bool found = false;
    UpdateAvailable available;
    if (!body.empty()) {
        std::shared_ptr<diskscan::JsonValue> root = diskscan::parseJson(body);
        const diskscan::JsonValue* latest = diskscan::member(root.get(), "latest");
        std::string version = diskscan::stringMember(latest, "version");
        if (!version.empty()) {
            std::wstring wideVersion = diskscan::fromUtf8(version);
            // The site does the comparison now that it runs per request; the
            // local one is the fallback if that field ever goes missing.
            const diskscan::JsonValue* flag = diskscan::member(root.get(), "updateAvailable");
            bool newer = (flag && flag->kind == diskscan::JsonValue::Kind::Bool)
                             ? flag->boolean
                             : isNewer(wideVersion, current);
            if (newer) {
                found = true;
                available.version = wideVersion;
                std::string downloads = diskscan::stringMember(root.get(), "downloads");
                available.downloads = downloads.empty() ? L"https://diskspace.io"
                                                        : diskscan::fromUtf8(downloads);
                available.currentReleaseDate =
                    diskscan::fromUtf8(diskscan::stringMember(root.get(), "currentDate"));
            }
        }
    }

    available_ = available;
    found_ = found;
    // Released last, so a reader that sees this set is guaranteed to see the
    // answer that was written before it.
    finished_.store(true, std::memory_order_release);
    if (found && notify) PostMessageW(notify, kMessage, 0, 0);
}

}  // namespace app

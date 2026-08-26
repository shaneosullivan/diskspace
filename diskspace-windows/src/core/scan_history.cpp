#include "scan_history.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>

#include "win_util.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace diskscan {
namespace ScanHistory {
namespace {

std::wstring storePath() {
    std::wstring dir = supportDirectory();
    if (dir.empty()) return std::wstring();
    return dir + L"\\scan-history.txt";
}

/// One record per line: epoch seconds, a tab, then the path to end of line.
/// The timestamp goes first so the path needs no escaping — a Windows path
/// cannot contain a newline, and everything after the first tab is the path.
std::map<std::wstring, uint32_t> load() {
    std::map<std::wstring, uint32_t> out;
    std::wstring path = storePath();
    if (path.empty()) return out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    std::string raw;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (8 << 20)) {
        raw.resize(size_t(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(h, raw.data(), DWORD(raw.size()), &read, nullptr)) read = 0;
        raw.resize(read);
    }
    CloseHandle(h);

    size_t pos = 0;
    while (pos < raw.size()) {
        size_t end = raw.find('\n', pos);
        if (end == std::string::npos) end = raw.size();
        std::string line = raw.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        uint32_t when = uint32_t(strtoul(line.substr(0, tab).c_str(), nullptr, 10));
        std::wstring root = fromUtf8(line.substr(tab + 1));
        if (when && !root.empty()) out[root] = when;
    }
    return out;
}

void save(const std::map<std::wstring, uint32_t>& entries) {
    std::wstring path = storePath();
    if (path.empty()) return;
    std::string body;
    for (const auto& e : entries) {
        body += std::to_string(e.second);
        body += '\t';
        body += toUtf8(e.first);
        body += '\n';
    }
    // Written to a sibling and moved into place, so a crash part way through
    // cannot leave a truncated file that loses every baseline.
    std::wstring temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, body.data(), DWORD(body.size()), &written, nullptr);
    CloseHandle(h);
    MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

/// Paths compare case-insensitively on Windows, so the key is folded once here
/// rather than every root being remembered separately per capitalisation.
std::wstring key(const std::wstring& root) {
    std::wstring out = root;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return wchar_t(towlower(c)); });
    return out;
}

}  // namespace

std::wstring supportDirectory() {
    PWSTR base = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        return std::wstring();
    }
    std::wstring dir = std::wstring(base) + L"\\DiskSpace";
    CoTaskMemFree(base);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

uint32_t lastScan(const std::wstring& root) {
    auto entries = load();
    auto it = entries.find(key(root));
    return it == entries.end() ? 0 : it->second;
}

void record(const std::wstring& root, uint32_t whenEpochSeconds) {
    auto entries = load();
    entries[key(root)] = whenEpochSeconds ? whenEpochSeconds : uint32_t(::time(nullptr));
    save(entries);
}

void forget(const std::wstring& root) {
    auto entries = load();
    entries.erase(key(root));
    save(entries);
}

}  // namespace ScanHistory
}  // namespace diskscan

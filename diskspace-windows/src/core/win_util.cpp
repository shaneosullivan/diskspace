#include "win_util.h"

#include <windows.h>

#include <algorithm>

#pragma comment(lib, "advapi32.lib")

namespace diskscan {

std::string toUtf8(const wchar_t* s, size_t len) {
    if (len == 0) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s, int(len), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, int(len), out.data(), n, nullptr, nullptr);
    return out;
}

std::string toUtf8(const std::wstring& s) { return toUtf8(s.c_str(), s.size()); }

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring canonicalPath(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    std::wstring input = path;
    // GetFullPathNameW keeps a trailing separator, which then shows up in every
    // child path as a doubled backslash. Drive roots keep theirs: "C:" alone
    // means "the current directory on C:", which is not what was asked for.
    while (input.size() > 3 && (input.back() == L'\\' || input.back() == L'/')) {
        input.pop_back();
    }
    DWORD n = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (n == 0) return std::wstring();
    std::wstring out(n, L'\0');
    DWORD written = GetFullPathNameW(input.c_str(), n, out.data(), nullptr);
    if (written == 0 || written >= n) return std::wstring();
    out.resize(written);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

std::wstring extendedPath(const std::wstring& path) {
    if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) return path;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        return L"\\\\?\\UNC\\" + path.substr(2);
    }
    return L"\\\\?\\" + path;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

/// Mount point of the volume holding `path`, with its trailing backslash.
static std::wstring volumeRoot(const std::wstring& path) {
    wchar_t buffer[MAX_PATH + 1] = {};
    std::wstring input = path;
    if (input.empty()) return std::wstring();
    if (input.back() != L'\\') input.push_back(L'\\');
    if (!GetVolumePathNameW(input.c_str(), buffer, MAX_PATH)) return std::wstring();
    return buffer;
}

uint32_t clusterSize(const std::wstring& path) {
    std::wstring root = volumeRoot(path);
    if (root.empty()) return 0;
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    if (!GetDiskFreeSpaceW(root.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters,
                           &totalClusters)) {
        return 0;
    }
    return uint32_t(sectorsPerCluster) * uint32_t(bytesPerSector);
}

static bool fillVolume(const std::wstring& root, VolumeInfo* out) {
    wchar_t name[MAX_PATH + 1] = {};
    wchar_t fs[MAX_PATH + 1] = {};
    if (!GetVolumeInformationW(root.c_str(), name, MAX_PATH, nullptr, nullptr, nullptr, fs,
                               MAX_PATH)) {
        return false;
    }
    ULARGE_INTEGER available{}, total{}, free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free)) return false;
    out->mountPoint = root;
    out->fileSystem = fs;
    out->supportsFastScan = (out->fileSystem == L"NTFS");
    out->totalBytes = int64_t(total.QuadPart);
    out->freeBytes = int64_t(free.QuadPart);
    out->clusterBytes = clusterSize(root);
    if (name[0]) out->mountPoint = root;
    return true;
}

bool volumeInfoForPath(const std::wstring& path, VolumeInfo* out) {
    std::wstring root = volumeRoot(path);
    if (root.empty()) return false;
    return fillVolume(root, out);
}

std::wstring volumeScanPath(const VolumeInfo& volume) { return volume.mountPoint; }

bool isVolumeRoot(const std::wstring& path) {
    VolumeInfo volume;
    if (!volumeInfoForPath(path, &volume)) return false;
    auto trimmed = [](std::wstring value) {
        while (value.size() > 1 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
        return value;
    };
    const std::wstring mount = trimmed(volume.mountPoint);
    const std::wstring target = trimmed(canonicalPath(path).empty() ? path : canonicalPath(path));
    return CompareStringOrdinal(target.c_str(), int(target.size()), mount.c_str(),
                                int(mount.size()), TRUE) == CSTR_EQUAL;
}

std::vector<VolumeInfo> mountedVolumes() {
    std::vector<VolumeInfo> out;
    DWORD mask = GetLogicalDrives();
    wchar_t systemRoot[MAX_PATH] = {};
    GetSystemDirectoryW(systemRoot, MAX_PATH);
    std::wstring systemDrive = volumeRoot(systemRoot);

    struct Ranked {
        int rank;
        VolumeInfo info;
    };
    std::vector<Ranked> ranked;
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        std::wstring root = std::wstring(1, wchar_t(L'A' + i)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        // No point offering a drive with nothing in it, and a CD or DVD is not
        // where anyone's disk space went.
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        VolumeInfo info;
        if (!fillVolume(root, &info)) continue;
        if (info.totalBytes <= 0) continue;
        // The system drive first, then other fixed disks, then anything removable
        // or networked. Sorting purely by capacity would put a large network share
        // above the disk the user is actually trying to free up.
        int rank = (root == systemDrive) ? 0 : (type == DRIVE_FIXED ? 1 : 2);
        ranked.push_back({rank, info});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        return a.info.totalBytes > b.info.totalBytes;
    });
    for (auto& r : ranked) out.push_back(r.info);
    return out;
}

}  // namespace diskscan

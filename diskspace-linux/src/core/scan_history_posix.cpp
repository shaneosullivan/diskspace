#include "scan_history.h"

#include <pwd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

#include "platform.h"

namespace diskscan {
namespace ScanHistory {
namespace {

NameString storePath() {
    NameString dir = supportDirectory();
    if (dir.empty()) return NameString();
    return dir + "/scan-history.txt";
}

/// One record per line: epoch seconds, a tab, then the path to end of line.
///
/// The Windows build can put the path in raw, because a Windows path cannot
/// contain a newline. A Linux one can - the kernel forbids only the separator
/// and the terminator - so a backslash and a newline are escaped on the way out
/// and restored on the way in. Without that, one awkwardly named directory would
/// silently corrupt every baseline in the file.
std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 >= s.size()) {
            out.push_back(s[i]);
            continue;
        }
        ++i;
        out.push_back(s[i] == 'n' ? '\n' : s[i]);
    }
    return out;
}

std::map<NameString, uint32_t> load() {
    std::map<NameString, uint32_t> out;
    const NameString path = storePath();
    if (path.empty()) return out;
    FILE* file = ::fopen(path.c_str(), "re");
    if (!file) return out;

    char line[8192];
    while (::fgets(line, sizeof(line), file)) {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        const size_t tab = text.find('\t');
        if (tab == std::string::npos) continue;
        const uint32_t when = uint32_t(::strtoul(text.substr(0, tab).c_str(), nullptr, 10));
        const NameString root = unescape(text.substr(tab + 1));
        if (when && !root.empty()) out[root] = when;
    }
    ::fclose(file);
    return out;
}

void save(const std::map<NameString, uint32_t>& entries) {
    const NameString path = storePath();
    if (path.empty()) return;
    std::string body;
    for (const auto& entry : entries) {
        body += std::to_string(entry.second);
        body += '\t';
        body += escape(entry.first);
        body += '\n';
    }
    // Written to a sibling and renamed over the original, so a crash part way
    // through cannot leave a truncated file that loses every baseline. rename is
    // atomic within a filesystem, which a sibling always is.
    const NameString temp = path + ".tmp";
    FILE* file = ::fopen(temp.c_str(), "we");
    if (!file) return;
    const bool ok = ::fwrite(body.data(), 1, body.size(), file) == body.size();
    ::fclose(file);
    if (ok) {
        ::rename(temp.c_str(), path.c_str());
    } else {
        ::unlink(temp.c_str());
    }
}

/// No case folding, unlike the Windows build. Linux paths are case-sensitive, so
/// /home/Shane and /home/shane are two different directories and remembering
/// them as one would report the wrong baseline for whichever was scanned second.
const NameString& key(const NameString& root) { return root; }

}  // namespace

NameString supportDirectory() {
    NameString base;
    if (const char* xdg = ::getenv("XDG_DATA_HOME"); xdg && *xdg == '/') {
        base = xdg;
    } else {
        const char* home = ::getenv("HOME");
        if (!home || !*home) {
            // No HOME in the environment - a daemon, or a stripped sudo. The
            // password database still knows.
            const struct passwd* pw = ::getpwuid(::getuid());
            if (!pw || !pw->pw_dir) return NameString();
            home = pw->pw_dir;
        }
        base = NameString(home) + "/.local/share";
    }

    // Each level in turn: ~/.local may not exist on a fresh account.
    const NameString dir = base + "/diskspace";
    NameString partial;
    for (size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        partial = dir.substr(0, i);
        ::mkdir(partial.c_str(), 0700);
    }
    struct stat st;
    if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return NameString();
    return dir;
}

uint32_t lastScan(const NameString& root) {
    const auto entries = load();
    const auto it = entries.find(key(root));
    return it == entries.end() ? 0 : it->second;
}

void record(const NameString& root, uint32_t whenEpochSeconds) {
    auto entries = load();
    entries[key(root)] = whenEpochSeconds ? whenEpochSeconds : uint32_t(::time(nullptr));
    save(entries);
}

void forget(const NameString& root) {
    auto entries = load();
    entries.erase(key(root));
    save(entries);
}

}  // namespace ScanHistory
}  // namespace diskscan

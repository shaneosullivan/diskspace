#include "preferences.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace app {
namespace {

constexpr const char* kGroup = "DiskSpace";

/// The settings, as they are held in memory.
///
/// A map rather than GKeyFile, which is the only thing this file used GLib for.
/// The format on disk is unchanged - the same INI group and the same keys - so a
/// settings file written by the GTK build is read by this one and the user's
/// preferences survive the switch.
using Table = std::map<std::string, std::string>;

Table& tableOf(void* handle) { return *static_cast<Table*>(handle); }

std::string trim(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && (value[start] == ' ' || value[start] == '\t')) ++start;
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(start, end - start);
}

/// $XDG_CONFIG_HOME/diskspace, created if needed.
std::string configDirectory() {
    std::string base;
    if (const char* xdg = ::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char* home = ::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        return std::string();
    }
    ::mkdir(base.c_str(), 0700);
    const std::string directory = base + "/diskspace";
    ::mkdir(directory.c_str(), 0700);
    return directory;
}

}  // namespace

Preferences& Preferences::shared() {
    static Preferences instance;
    return instance;
}

Preferences::Preferences() {
    const std::string directory = configDirectory();
    if (!directory.empty()) path_ = directory + "/settings.ini";
    keyFile_ = new Table();
    load();
}

void Preferences::load() {
    if (path_.empty()) return;
    std::ifstream file(path_);
    // A missing file is the ordinary first-run case, not an error: every getter
    // falls back to its default.
    if (!file) return;

    Table& table = tableOf(keyFile_);
    std::string line;
    bool inGroup = false;
    while (std::getline(file, line)) {
        const std::string text = trim(line);
        if (text.empty() || text[0] == '#' || text[0] == ';') continue;
        if (text.front() == '[' && text.back() == ']') {
            inGroup = text.substr(1, text.size() - 2) == kGroup;
            continue;
        }
        if (!inGroup) continue;
        const size_t equals = text.find('=');
        if (equals == std::string::npos) continue;
        table[trim(text.substr(0, equals))] = trim(text.substr(equals + 1));
    }
}

void Preferences::save() const {
    if (path_.empty()) return;

    // Through a temporary and a rename, which is what g_key_file_save_to_file
    // did and worth keeping: a rename is atomic, so an interrupted write cannot
    // leave a half-file where the settings used to be.
    const std::string temporary = path_ + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return;
        file << "[" << kGroup << "]\n";
        for (const auto& [key, value] : tableOf(keyFile_)) file << key << "=" << value << "\n";
        if (!file.good()) return;
    }
    if (::rename(temporary.c_str(), path_.c_str()) != 0) ::unlink(temporary.c_str());
}

bool Preferences::flag(const char* key, bool fallback) const {
    const Table& table = tableOf(keyFile_);
    const auto found = table.find(key);
    // Absent, or present and not a boolean. Either way the default is the better
    // answer than false.
    if (found == table.end()) return fallback;
    if (found->second == "true" || found->second == "1") return true;
    if (found->second == "false" || found->second == "0") return false;
    return fallback;
}

void Preferences::set(const char* key, bool value) {
    if (flag(key, !value) == value) return;  // nothing changed, nobody to tell
    tableOf(keyFile_)[key] = value ? "true" : "false";
    save();
    notify();
}

void Preferences::resetAll() {
    tableOf(keyFile_).clear();
    save();
    notify();
}

void Preferences::addObserver(std::function<void()> observer) {
    observers_.push_back(std::move(observer));
}

void Preferences::notify() {
    for (const auto& observer : observers_) observer();
}

}  // namespace app

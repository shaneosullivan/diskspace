#include "preferences.h"

#include <glib.h>

namespace app {
namespace {

constexpr const char* kGroup = "DiskSpace";

/// $XDG_CONFIG_HOME/diskspace, created if needed.
std::string configDirectory() {
    const char* base = g_get_user_config_dir();  // honours XDG_CONFIG_HOME
    if (!base || !*base) return std::string();
    char* dir = g_build_filename(base, "diskspace", nullptr);
    g_mkdir_with_parents(dir, 0700);
    std::string out(dir);
    g_free(dir);
    return out;
}

}  // namespace

Preferences& Preferences::shared() {
    static Preferences instance;
    return instance;
}

Preferences::Preferences() {
    const std::string dir = configDirectory();
    if (!dir.empty()) {
        char* path = g_build_filename(dir.c_str(), "settings.ini", nullptr);
        path_ = path;
        g_free(path);
    }
    keyFile_ = g_key_file_new();
    load();
}

void Preferences::load() {
    if (path_.empty()) return;
    // A missing file is the ordinary first-run case, not an error: every getter
    // falls back to its default.
    g_key_file_load_from_file(static_cast<GKeyFile*>(keyFile_), path_.c_str(),
                              G_KEY_FILE_NONE, nullptr);
}

void Preferences::save() const {
    if (path_.empty()) return;
    // g_key_file_save_to_file writes through a temporary and renames, so an
    // interrupted write cannot leave a half-file behind.
    g_key_file_save_to_file(static_cast<GKeyFile*>(keyFile_), path_.c_str(), nullptr);
}

bool Preferences::flag(const char* key, bool fallback) const {
    GError* error = nullptr;
    const gboolean value =
        g_key_file_get_boolean(static_cast<GKeyFile*>(keyFile_), kGroup, key, &error);
    if (error) {
        // Absent, or present and not a boolean. Either way the default is the
        // better answer than false.
        g_error_free(error);
        return fallback;
    }
    return value != FALSE;
}

void Preferences::set(const char* key, bool value) {
    if (flag(key, !value) == value) return;  // nothing changed, nobody to tell
    g_key_file_set_boolean(static_cast<GKeyFile*>(keyFile_), kGroup, key, value ? TRUE : FALSE);
    save();
    notify();
}

void Preferences::resetAll() {
    g_key_file_free(static_cast<GKeyFile*>(keyFile_));
    keyFile_ = g_key_file_new();
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

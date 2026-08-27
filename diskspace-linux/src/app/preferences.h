#pragma once

#include <functional>
#include <string>
#include <vector>

namespace app {

/// Application settings, kept in $XDG_CONFIG_HOME/diskspace/settings.ini.
///
/// A plain key file rather than GSettings, which would need a compiled schema
/// installed system-wide before the program would start at all - fine for a
/// distribution package, hostile to a single downloaded binary, which is how
/// most people will first meet this.
///
/// Every change notifies the observers, which the window uses to re-apply
/// layout, colours and the treemap without needing a rescan.
class Preferences {
public:
    static Preferences& shared();

    /// Space actually occupied on disk, rather than the length of a file's
    /// contents. The two differ sharply where sparse files and filesystem
    /// compression are involved.
    bool showPhysicalFileSize() const { return flag("ShowPhysicalFileSize", true); }
    void setShowPhysicalFileSize(bool v) { set("ShowPhysicalFileSize", v); }

    /// List above map rather than beside it.
    bool splitHorizontally() const { return flag("SplitHorizontally", false); }
    void setSplitHorizontally(bool v) { set("SplitHorizontally", v); }

    /// Derive each kind's colour from the kind itself, so a given file type looks
    /// the same in every scan. Turned off, colours are assigned by size rank
    /// within the current scan, which gives the largest kinds the most distinct
    /// hues.
    bool consistentKindColours() const { return flag("ConsistentKindColours", true); }
    void setConsistentKindColours(bool v) { set("ConsistentKindColours", v); }

    /// Draw the volume's free space as a block, to size the scanned folder
    /// against the room left on it.
    bool showFreeSpace() const { return flag("ShowFreeSpace", false); }
    void setShowFreeSpace(bool v) { set("ShowFreeSpace", v); }

    /// Draw everything else in use on the volume as a block. Meaningless when a
    /// whole volume is being shown, and hidden in that case.
    bool showOtherSpace() const { return flag("ShowOtherSpace", false); }
    void setShowOtherSpace(bool v) { set("ShowOtherSpace", v); }

    bool showFileKinds() const { return flag("ShowFileKinds", true); }
    void setShowFileKinds(bool v) { set("ShowFileKinds", v); }

    /// Ask the site at launch whether a newer version exists. This is the only
    /// network request the program ever makes, so it is the user's to switch off.
    bool checkForUpdates() const { return flag("CheckForUpdates", true); }
    void setCheckForUpdates(bool v) { set("CheckForUpdates", v); }

    void resetAll();

    void addObserver(std::function<void()> observer);

private:
    Preferences();
    bool flag(const char* key, bool fallback) const;
    void set(const char* key, bool value);
    void load();
    void save() const;
    void notify();

    std::string path_;
    /// Loaded once and written on every change. The file holds a dozen booleans;
    /// there is nothing here worth a more careful arrangement than that.
    void* keyFile_ = nullptr;
    std::vector<std::function<void()>> observers_;
};

}  // namespace app

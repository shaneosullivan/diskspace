#pragma once

#include <functional>
#include <string>
#include <vector>

namespace app {

/// Application settings, persisted under HKCU\Software\DiskSpace.
///
/// Every change notifies the observers, which the main window uses to re-apply
/// layout, fonts, colours and the treemap without needing a rescan.
class Preferences {
public:
    static Preferences& shared();

    /// Descend into a folder's contents rather than treating it as one item.
    /// Windows has no equivalent of a macOS bundle, so unlike the Mac version
    /// there is no package setting here.

    /// Space actually occupied on the drive, rather than the length of a file's
    /// contents. The two differ sharply where compression and sparse files are
    /// involved.
    bool showPhysicalFileSize() const { return flag(L"ShowPhysicalFileSize", true); }
    void setShowPhysicalFileSize(bool v) { set(L"ShowPhysicalFileSize", v); }

    bool splitHorizontally() const { return flag(L"SplitHorizontally", false); }
    void setSplitHorizontally(bool v) { set(L"SplitHorizontally", v); }

    bool smallFontFiles() const { return flag(L"SmallFontFiles", false); }
    void setSmallFontFiles(bool v) { set(L"SmallFontFiles", v); }

    bool smallFontKinds() const { return flag(L"SmallFontKinds", false); }
    void setSmallFontKinds(bool v) { set(L"SmallFontKinds", v); }

    /// Derive each kind's colour from the kind itself, so a given file type looks
    /// the same in every scan. Turned off, colours are assigned by size rank
    /// within the current scan, which gives the largest kinds the most distinct
    /// hues.
    bool consistentKindColours() const { return flag(L"ConsistentKindColours", true); }
    void setConsistentKindColours(bool v) { set(L"ConsistentKindColours", v); }

    bool animatedZooming() const { return flag(L"AnimatedZooming", true); }
    void setAnimatedZooming(bool v) { set(L"AnimatedZooming", v); }

    /// Draw the drive's free space as a block, to size the scanned folder against
    /// the room left on the drive.
    bool showFreeSpace() const { return flag(L"ShowFreeSpace", false); }
    void setShowFreeSpace(bool v) { set(L"ShowFreeSpace", v); }

    /// Draw everything else in use on the drive as a block. Meaningless when a
    /// whole drive is being shown, and hidden in that case.
    bool showOtherSpace() const { return flag(L"ShowOtherSpace", false); }
    void setShowOtherSpace(bool v) { set(L"ShowOtherSpace", v); }

    bool showFileKindStatistics() const { return flag(L"ShowFileKindStatistics", true); }
    void setShowFileKindStatistics(bool v) { set(L"ShowFileKindStatistics", v); }

    bool showSelectionList() const { return flag(L"ShowSelectionList", false); }
    void setShowSelectionList(bool v) { set(L"ShowSelectionList", v); }

    bool showInformation() const { return flag(L"ShowInformation", false); }
    void setShowInformation(bool v) { set(L"ShowInformation", v); }

    /// Offer to relaunch as administrator when the fast backend would help. Turned
    /// off by anyone who would rather never see the prompt again.
    bool offerElevation() const { return flag(L"OfferElevation", true); }
    void setOfferElevation(bool v) { set(L"OfferElevation", v); }

    /// Ask the site at launch whether a newer version exists. This is the only
    /// network request the app ever makes, so it is the user's to switch off.
    bool checkForUpdates() const { return flag(L"CheckForUpdates", true); }
    void setCheckForUpdates(bool v) { set(L"CheckForUpdates", v); }

    void resetAll();

    void addObserver(std::function<void()> observer);

private:
    Preferences() = default;
    bool flag(const wchar_t* key, bool fallback) const;
    void set(const wchar_t* key, bool value);
    void notify();

    std::vector<std::function<void()>> observers_;
};

}  // namespace app

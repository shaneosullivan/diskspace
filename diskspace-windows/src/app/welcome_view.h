#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../core/win_util.h"
#include "ui_common.h"

namespace app {

/// A folder or volume offered on the chooser.
struct ScanSuggestion {
    std::wstring title;
    std::wstring subtitle;
    std::wstring path;
    /// Capacity and free bytes, for volumes only.
    bool isVolume = false;
    int64_t capacity = 0;
    int64_t free = 0;
    /// The shell's own icon for this path, so a drive looks like that drive and
    /// a folder like that folder. Owned by the view, which releases it on reload.
    HICON icon = nullptr;
};

namespace SuggestionSource {
/// Mounted drives, the system drive first.
std::vector<ScanSuggestion> volumes();
/// Folders that are usually where the space actually went.
std::vector<ScanSuggestion> folders();
}  // namespace SuggestionSource

/// The chooser, shown before anything has been scanned and returned to whenever
/// the user asks to open something else.
class WelcomeView {
public:
    static WelcomeView* create(HWND parent, int id);
    HWND window() const { return window_; }

    void reload();

    /// Shows or clears the "a newer version is available" banner. Null hides it.
    void setUpdate(const struct UpdateAvailable* update);

    std::function<void(const std::wstring&)> onPick;
    std::function<void()> onBrowse;
    /// Asked when the administrator banner's button is pressed.
    std::function<void()> onRelaunchElevated;
    /// Asked when the update banner's button is pressed.
    std::function<void(const std::wstring& downloadsUrl)> onDownloadUpdate;

private:
    struct Item {
        ScanSuggestion suggestion;
        RECT bounds{};
        bool isHeader = false;
        std::wstring headerText;
    };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    /// The two links in the footer, and where they landed.
    struct FooterLink {
        RECT bounds{};
        const wchar_t* text = nullptr;
        const wchar_t* url = nullptr;
    };

    void paint(HDC dc);
    /// Index into footerLinks_ under the point, or -1. `y` is in client
    /// coordinates, so the scroll offset is applied here rather than by callers.
    int footerHitTest(int x, int y) const;
    void paintCard(HDC dc, const Item& item, bool hovered, int offset);
    void layout();
    int hitTest(int x, int y) const;
    /// Icons come from the shell and have to be given back.
    void releaseIcons();

    FooterLink footerLinks_[2];
    RECT footerBounds_{};
    int hoveredFooterLink_ = -1;

    HWND window_ = nullptr;
    std::vector<Item> items_;
    std::vector<ScanSuggestion> volumes_;
    std::vector<ScanSuggestion> folders_;
    RECT browseButton_{};
    RECT elevateButton_{};
    RECT updateButton_{};
    /// Laid out in flow rather than at fixed offsets, so either banner can be
    /// absent without leaving a gap where it would have been.
    RECT elevationBanner_{};
    RECT updateBanner_{};
    /// The heading row: the app icon and the title beside it.
    RECT headerBounds_{};
    HICON appIcon_ = nullptr;
    bool showElevationBanner_ = false;
    /// Non-empty when a newer version exists: the sentence to show and where to
    /// send anyone who wants it.
    std::wstring updateSummary_;
    std::wstring updateUrl_;
    int hovered_ = -1;
    bool hoveringBrowse_ = false;
    bool hoveringElevate_ = false;
    bool hoveringUpdate_ = false;
    int scrollY_ = 0;
    int contentHeight_ = 0;
    ui::BackBuffer buffer_;
};

}  // namespace app

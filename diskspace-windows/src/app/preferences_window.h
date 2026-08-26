#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "ui_common.h"

namespace app {

/// The settings window: two pages, General and Treemap.
///
/// Drawn rather than assembled from dialog controls, for the same reason the
/// panes are: a stock checkbox keeps its light face in a dark window, and the
/// explanation under each setting wants to be laid out with it rather than
/// positioned by hand in a resource script.
class PreferencesWindow {
public:
    static PreferencesWindow& shared();

    void show(HWND owner);
    bool visible() const;

private:
    /// Which preference a row reads and writes. Kept separate from the row list
    /// so the layout carries no behaviour.
    enum class Setting {
        None,
        PhysicalFileSize,
        SplitHorizontally,
        SmallFontFiles,
        SmallFontKinds,
        ConsistentColours,
        OfferElevation,
        CheckForUpdates,
        AnimatedZooming,
        ShowFreeSpace,
        ShowOtherSpace,
    };

    enum class Page { General, Treemap };

    struct Row {
        enum class Kind { Checkbox, Separator } kind = Kind::Checkbox;
        Page page = Page::General;
        /// Right-aligned label beside the control, on the first row of a group.
        std::wstring caption;
        std::wstring title;
        std::wstring note;
        Setting setting = Setting::None;

        // Filled in by layout.
        RECT bounds{};
        RECT control{};
    };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);
    void buildRows();
    void layout();
    void paint(HDC dc);
    void paintRow(HDC dc, const Row& row);
    int rowAt(int x, int y) const;
    int tabAt(int x, int y) const;
    static bool read(Setting setting);
    static void write(Setting setting, bool value);

    HWND window_ = nullptr;
    Page page_ = Page::General;
    std::vector<Row> rows_;
    RECT resetButton_{};
    int hoveredRow_ = -1;
    int hoveredTab_ = -1;
    bool hoveringReset_ = false;
    ui::BackBuffer buffer_;
};

}  // namespace app

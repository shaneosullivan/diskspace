#include "preferences_window.h"

#include <windowsx.h>

#include <algorithm>

#include "preferences.h"

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpacePreferencesWindow";
constexpr int kWindowWidth = 720;
constexpr int kWindowHeight = 620;
/// Reserved at the bottom for the Reset button, so a long note can never end up
/// underneath it.
constexpr int kFooterHeight = 56;
constexpr int kSidebarWidth = 150;
constexpr int kCaptionWidth = 170;
constexpr int kControlLeft = 190;
constexpr int kCheckboxSize = 15;

/// A tick, drawn rather than glyphed so it stays crisp at every scaling factor.
void drawTick(HDC dc, const RECT& box, COLORREF colour) {
    HPEN pen = CreatePen(PS_SOLID, (std::max)(2, int(box.right - box.left) / 7), colour);
    HPEN previous = HPEN(SelectObject(dc, pen));
    int width = box.right - box.left;
    POINT points[3] = {
        {box.left + width / 5, box.top + width / 2},
        {box.left + width / 2 - width / 12, box.bottom - width / 4},
        {box.right - width / 6, box.top + width / 4},
    };
    Polyline(dc, points, 3);
    SelectObject(dc, previous);
    DeleteObject(pen);
}

}  // namespace

PreferencesWindow& PreferencesWindow::shared() {
    static PreferencesWindow instance;
    return instance;
}

// MARK: - The settings themselves

bool PreferencesWindow::read(Setting setting) {
    Preferences& p = Preferences::shared();
    switch (setting) {
        case Setting::PhysicalFileSize: return p.showPhysicalFileSize();
        case Setting::SplitHorizontally: return p.splitHorizontally();
        case Setting::SmallFontFiles: return p.smallFontFiles();
        case Setting::SmallFontKinds: return p.smallFontKinds();
        case Setting::ConsistentColours: return p.consistentKindColours();
        case Setting::OfferElevation: return p.offerElevation();
        case Setting::CheckForUpdates: return p.checkForUpdates();
        case Setting::AnimatedZooming: return p.animatedZooming();
        case Setting::ShowFreeSpace: return p.showFreeSpace();
        case Setting::ShowOtherSpace: return p.showOtherSpace();
        default: return false;
    }
}

void PreferencesWindow::write(Setting setting, bool value) {
    Preferences& p = Preferences::shared();
    switch (setting) {
        case Setting::PhysicalFileSize: p.setShowPhysicalFileSize(value); break;
        case Setting::SplitHorizontally: p.setSplitHorizontally(value); break;
        case Setting::SmallFontFiles: p.setSmallFontFiles(value); break;
        case Setting::SmallFontKinds: p.setSmallFontKinds(value); break;
        case Setting::ConsistentColours: p.setConsistentKindColours(value); break;
        case Setting::OfferElevation: p.setOfferElevation(value); break;
        case Setting::CheckForUpdates: p.setCheckForUpdates(value); break;
        case Setting::AnimatedZooming: p.setAnimatedZooming(value); break;
        case Setting::ShowFreeSpace: p.setShowFreeSpace(value); break;
        case Setting::ShowOtherSpace: p.setShowOtherSpace(value); break;
        default: break;
    }
}

// MARK: - Content

void PreferencesWindow::buildRows() {
    rows_.clear();
    auto checkbox = [&](Page page, const wchar_t* caption, const wchar_t* title, Setting setting,
                        const wchar_t* note) {
        Row row;
        row.kind = Row::Kind::Checkbox;
        row.page = page;
        row.caption = caption ? caption : L"";
        row.title = title;
        row.note = note ? note : L"";
        row.setting = setting;
        rows_.push_back(row);
    };
    auto separator = [&](Page page) {
        Row row;
        row.kind = Row::Kind::Separator;
        row.page = page;
        rows_.push_back(row);
    };

    checkbox(Page::General, L"View settings:", L"Show physical file size", Setting::PhysicalFileSize,
             L"The physical size is the space a file occupies on the drive. Compressed and "
             L"sparse files can hold far more content than they occupy.");
    checkbox(Page::General, nullptr, L"Split window horizontally", Setting::SplitHorizontally,
             nullptr);
    separator(Page::General);
    checkbox(Page::General, L"Use small font in:", L"Files view", Setting::SmallFontFiles, nullptr);
    checkbox(Page::General, nullptr, L"File kinds", Setting::SmallFontKinds, nullptr);
    separator(Page::General);
    checkbox(Page::General, nullptr, L"Use the same colour for each file kind in all scans",
             Setting::ConsistentColours, nullptr);
    separator(Page::General);
    checkbox(Page::General, L"Scanning:", L"Offer to restart as administrator",
             Setting::OfferElevation,
             L"Reading the drive's file table directly scans a whole drive in seconds, and needs "
             L"administrator rights. Only ever offered, never required — the figures are the same "
             L"either way.");

    separator(Page::General);
    checkbox(Page::General, L"Updates:", L"Check for a new version at launch",
             Setting::CheckForUpdates,
             L"Asks diskspace.io whether a newer version has been released, sending only the "
             L"platform and the version running. Nothing about what was scanned ever leaves the "
             L"machine.");

    checkbox(Page::Treemap, nullptr, L"Animated zooming", Setting::AnimatedZooming,
             L"Turn this off if the animation is slow on your machine, or you dislike it.");
    separator(Page::Treemap);
    checkbox(Page::Treemap, L"View settings:", L"Show free space", Setting::ShowFreeSpace,
             L"Shows free space as a block in the treemap, so the scanned folder can be sized "
             L"against the room left on the drive.");
    checkbox(Page::Treemap, nullptr, L"Show other space", Setting::ShowOtherSpace,
             L"Shows everything else in use on the drive as a block. Scanning a whole drive, that "
             L"block is what the scan could not reach: folders closed even to an administrator, "
             L"and the file system's own records.");
}

void PreferencesWindow::layout() {
    RECT client;
    GetClientRect(window_, &client);
    const int left = ui::scaled(window_, kSidebarWidth);
    const int controlLeft = left + ui::scaled(window_, kControlLeft);
    const int right = client.right - ui::scaled(window_, 24);
    const int checkbox = ui::scaled(window_, kCheckboxSize);

    HDC dc = GetDC(window_);
    HFONT small = ui::font(window_, ui::FontRole::Small);

    int y = ui::scaled(window_, 24);
    for (Row& row : rows_) {
        if (row.page != page_) continue;
        int top = y;
        if (row.kind == Row::Kind::Separator) {
            y += ui::scaled(window_, 18);
            row.bounds = {left, top, right, y};
            continue;
        }

        int rowHeight = ui::scaled(window_, 22);
        row.control = {controlLeft, top + (rowHeight - checkbox) / 2, controlLeft + checkbox,
                       top + (rowHeight - checkbox) / 2 + checkbox};
        y += rowHeight;

        if (!row.note.empty()) {
            RECT measure{controlLeft + ui::scaled(window_, 20), 0, right, 0};
            HFONT previous = HFONT(SelectObject(dc, small));
            DrawTextW(dc, row.note.c_str(), int(row.note.size()), &measure,
                      DT_CALCRECT | DT_WORDBREAK);
            SelectObject(dc, previous);
            y += (measure.bottom - measure.top) + ui::scaled(window_, 6);
        }
        y += ui::scaled(window_, 8);
        row.bounds = {left, top, right, y};
    }
    ReleaseDC(window_, dc);

    int buttonWidth = ui::scaled(window_, 96);
    int buttonHeight = ui::scaled(window_, 28);
    int footerTop = client.bottom - ui::scaled(window_, kFooterHeight);
    resetButton_ = {left + ui::scaled(window_, 24), footerTop + (ui::scaled(window_, kFooterHeight) - buttonHeight) / 2,
                    left + ui::scaled(window_, 24) + buttonWidth,
                    footerTop + (ui::scaled(window_, kFooterHeight) - buttonHeight) / 2 + buttonHeight};
}

// MARK: - Painting

void PreferencesWindow::paintRow(HDC dc, const Row& row) {
    const ui::Theme& t = ui::theme();
    HFONT body = ui::font(window_, ui::FontRole::Body);
    HFONT small = ui::font(window_, ui::FontRole::Small);

    if (row.kind == Row::Kind::Separator) {
        int mid = (row.bounds.top + row.bounds.bottom) / 2;
        RECT line{row.bounds.left + ui::scaled(window_, 24), mid, row.bounds.right, mid + 1};
        ui::fill(dc, line, t.separator);
        return;
    }

    if (!row.caption.empty()) {
        RECT caption{row.bounds.left + ui::scaled(window_, 10), row.control.top - ui::scaled(window_, 2),
                     row.bounds.left + ui::scaled(window_, kCaptionWidth), row.control.bottom};
        ui::drawText(dc, row.caption, caption, t.secondaryText, body,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    bool on = read(row.setting);
    ui::fill(dc, row.control, on ? t.selection : t.paneBackground);
    ui::frame(dc, row.control, on ? t.selection : t.separator, 1);
    if (on) drawTick(dc, row.control, t.selectionText);

    RECT label{row.control.right + ui::scaled(window_, 9), row.control.top - ui::scaled(window_, 3),
               row.bounds.right, row.control.bottom + ui::scaled(window_, 3)};
    ui::drawText(dc, row.title, label, t.text, body, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (!row.note.empty()) {
        RECT note{row.control.left + ui::scaled(window_, 20), row.control.bottom + ui::scaled(window_, 5),
                  row.bounds.right, row.bounds.bottom};
        ui::drawText(dc, row.note, note, t.secondaryText, small, DT_LEFT | DT_WORDBREAK);
    }
}

void PreferencesWindow::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    HDC dc = buffer_.begin(target, client.right, client.bottom);
    if (!dc) return;

    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    // Sidebar.
    const int left = ui::scaled(window_, kSidebarWidth);
    RECT sidebar{0, 0, left, client.bottom};
    ui::fill(dc, sidebar, t.windowBackground);
    RECT edge{left - 1, 0, left, client.bottom};
    ui::fill(dc, edge, t.separator);

    HFONT body = ui::font(window_, ui::FontRole::Body);
    const wchar_t* titles[] = {L"General", L"Treemap"};
    int tabHeight = ui::scaled(window_, 34);
    for (int i = 0; i < 2; ++i) {
        RECT tab{ui::scaled(window_, 8), ui::scaled(window_, 18) + i * (tabHeight + 4),
                 left - ui::scaled(window_, 8), 0};
        tab.bottom = tab.top + tabHeight;
        bool selected = int(page_) == i;
        if (selected) {
            ui::fill(dc, tab, t.selection);
        } else if (hoveredTab_ == i) {
            ui::fill(dc, tab, t.hover);
        }
        RECT label = tab;
        label.left += ui::scaled(window_, 14);
        ui::drawText(dc, titles[i], label, selected ? t.selectionText : t.text, body,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    for (const Row& row : rows_) {
        if (row.page == page_) paintRow(dc, row);
    }

    int footerTop = client.bottom - ui::scaled(window_, kFooterHeight);
    RECT footerEdge{left, footerTop, client.right, footerTop + 1};
    ui::fill(dc, footerEdge, t.separator);
    ui::fill(dc, resetButton_, hoveringReset_ ? t.hover : t.paneBackground);
    ui::frame(dc, resetButton_, t.separator, 1);
    ui::drawText(dc, L"Reset", resetButton_, t.text, body,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT resetNote{resetButton_.right + ui::scaled(window_, 12), resetButton_.top,
                   client.right - ui::scaled(window_, 24), resetButton_.bottom};
    ui::drawText(dc, L"Puts every setting back to how it started.", resetNote, t.secondaryText,
                 ui::font(window_, ui::FontRole::Small), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    buffer_.blit(target, client.right, client.bottom);
}

// MARK: - Hit testing

int PreferencesWindow::rowAt(int x, int y) const {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (row.page != page_ || row.kind == Row::Kind::Separator) continue;
        if (y < row.bounds.top || y >= row.bounds.bottom) continue;
        // The whole row is the target, not just the box: a fifteen-pixel square
        // is a hard thing to hit, and the label beside it reads as part of it.
        if (x >= row.control.left && x < row.bounds.right && y >= row.control.top - 4 &&
            y < row.control.bottom + 4) {
            return int(i);
        }
        return -1;
    }
    return -1;
}

int PreferencesWindow::tabAt(int x, int y) const {
    if (x >= ui::scaled(window_, kSidebarWidth)) return -1;
    int tabHeight = ui::scaled(window_, 34);
    for (int i = 0; i < 2; ++i) {
        int top = ui::scaled(window_, 18) + i * (tabHeight + 4);
        if (y >= top && y < top + tabHeight) return i;
    }
    return -1;
}

// MARK: - Window

void PreferencesWindow::show(HWND owner) {
    if (!window_) {
        ui::registerClass(kClassName, proc);
        RECT desired{0, 0, kWindowWidth, kWindowHeight};
        AdjustWindowRectEx(&desired, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
        window_ = CreateWindowExW(0, kClassName, L"Settings",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                  CW_USEDEFAULT, desired.right - desired.left,
                                  desired.bottom - desired.top, owner, nullptr,
                                  GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(window_, GWLP_USERDATA, LONG_PTR(this));
        ui::applySystemTheme(window_);
        buildRows();
        layout();
    }
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
}

bool PreferencesWindow::visible() const { return window_ && IsWindowVisible(window_); }

LRESULT CALLBACK PreferencesWindow::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<PreferencesWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!self) return DefWindowProcW(window, message, w, l);
    self->window_ = window;
    return self->handle(message, w, l);
}

LRESULT PreferencesWindow::handle(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int row = rowAt(x, y);
            int tab = tabAt(x, y);
            bool reset = PtInRect(&resetButton_, POINT{x, y}) != 0;
            if (row != hoveredRow_ || tab != hoveredTab_ || reset != hoveringReset_) {
                hoveredRow_ = row;
                hoveredTab_ = tab;
                hoveringReset_ = reset;
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (hoveredRow_ >= 0 || hoveredTab_ >= 0 || hoveringReset_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return 1;
            }
            break;
        case WM_MOUSELEAVE:
            hoveredRow_ = hoveredTab_ = -1;
            hoveringReset_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int tab = tabAt(x, y);
            if (tab >= 0) {
                page_ = Page(tab);
                layout();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&resetButton_, POINT{x, y})) {
                Preferences::shared().resetAll();
                layout();
                InvalidateRect(window_, nullptr, FALSE);
                return 0;
            }
            int index = rowAt(x, y);
            if (index < 0) return 0;
            write(rows_[size_t(index)].setting, !read(rows_[size_t(index)].setting));
            // A note can appear or disappear with a setting, so the page is laid
            // out again rather than only redrawn.
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) ShowWindow(window_, SW_HIDE);
            return 0;
        case WM_DPICHANGED:
            layout();
            InvalidateRect(window_, nullptr, TRUE);
            return 0;
        case WM_CLOSE:
            ShowWindow(window_, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app

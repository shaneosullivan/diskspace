#include "welcome_view.h"

#include "resource.h"
#include "update_check.h"

#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>

namespace app {
namespace {

constexpr const wchar_t* kClassName = L"DiskSpaceWelcomeView";
constexpr int kRowHeight = 58;
constexpr int kContentWidth = 760;
constexpr int kGutter = 24;
/// The app icon beside the heading, at the size the artwork was drawn for.
constexpr int kHeaderIcon = 44;

std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &path))) return std::wstring();
    std::wstring out = path;
    CoTaskMemFree(path);
    return out;
}

/// The shell's own icon for a path. Caller owns it.
HICON shellIcon(const std::wstring& path) {
    SHFILEINFOW info{};
    // SHGFI_USEFILEATTRIBUTES is deliberately not passed: the point is the real
    // icon for this drive or folder, which means the shell has to look at it.
    if (!SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_ICON | SHGFI_LARGEICON)) {
        return nullptr;
    }
    return info.hIcon;
}

bool directoryExists(const std::wstring& path) {
    if (path.empty()) return false;
    DWORD attributes = GetFileAttributesW(diskscan::extendedPath(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

namespace SuggestionSource {

std::vector<ScanSuggestion> volumes() {
    std::vector<ScanSuggestion> out;
    for (const diskscan::VolumeInfo& volume : diskscan::mountedVolumes()) {
        ScanSuggestion suggestion;
        std::wstring mount = volume.mountPoint;
        while (mount.size() > 1 && mount.back() == L'\\') mount.pop_back();
        suggestion.title = mount;
        suggestion.path = mount;
        suggestion.isVolume = true;
        suggestion.capacity = volume.totalBytes;
        suggestion.free = volume.freeBytes;
        suggestion.subtitle = volume.fileSystem;
        suggestion.icon = shellIcon(volume.mountPoint);
        out.push_back(suggestion);
    }
    return out;
}

std::vector<ScanSuggestion> folders() {
    std::wstring profile = knownFolder(FOLDERID_Profile);
    const struct {
        const wchar_t* title;
        const wchar_t* subtitle;
        std::wstring path;
    } candidates[] = {
        {L"Home Folder", L"Everything under your user account", profile},
        {L"Downloads", L"Often the fastest win", knownFolder(FOLDERID_Downloads)},
        {L"AppData", L"Caches and application data, and usually larger than it looks",
         knownFolder(FOLDERID_LocalAppData)},
        {L"Documents", L"", knownFolder(FOLDERID_Documents)},
        {L"Program Files", L"Installed applications", knownFolder(FOLDERID_ProgramFiles)},
        {L"Windows", L"The system itself, including update and component stores",
         knownFolder(FOLDERID_Windows)},
    };
    std::vector<ScanSuggestion> out;
    for (const auto& candidate : candidates) {
        if (!directoryExists(candidate.path)) continue;
        ScanSuggestion suggestion;
        suggestion.title = candidate.title;
        suggestion.subtitle = candidate.subtitle;
        suggestion.path = candidate.path;
        suggestion.icon = shellIcon(candidate.path);
        out.push_back(suggestion);
    }
    return out;
}

}  // namespace SuggestionSource

WelcomeView* WelcomeView::create(HWND parent, int id) {
    ui::registerClass(kClassName, proc, LoadCursorW(nullptr, IDC_ARROW));
    auto* view = new WelcomeView();
    view->window_ = CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0,
                                    0, parent, HMENU(intptr_t(id)), GetModuleHandleW(nullptr), view);
    // Asked for at the size it is drawn at, so the shell picks the frame closest
    // to it out of the icon rather than scaling one that does not fit.
    int size = ui::scaled(view->window_, kHeaderIcon);
    view->appIcon_ = HICON(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON),
                                      IMAGE_ICON, size, size, 0));
    return view;
}

LRESULT CALLBACK WelcomeView::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<WelcomeView*>(create->lpCreateParams)->window_ = window;
    }
    auto* view = reinterpret_cast<WelcomeView*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!view) return DefWindowProcW(window, message, w, l);
    return view->handle(message, w, l);
}

void WelcomeView::releaseIcons() {
    for (auto* list : {&volumes_, &folders_}) {
        for (ScanSuggestion& suggestion : *list) {
            if (suggestion.icon) DestroyIcon(suggestion.icon);
            suggestion.icon = nullptr;
        }
    }
}

void WelcomeView::reload() {
    releaseIcons();
    volumes_ = SuggestionSource::volumes();
    folders_ = SuggestionSource::folders();
    // Only worth saying when it would actually change something: on a machine
    // with no NTFS volume the fast backend is not available at any privilege.
    bool anyNtfs = false;
    for (const diskscan::VolumeInfo& volume : diskscan::mountedVolumes()) {
        if (volume.isNtfs) anyNtfs = true;
    }
    showElevationBanner_ = anyNtfs && !diskscan::isElevated();
    layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void WelcomeView::setUpdate(const UpdateAvailable* update) {
    if (update) {
        updateSummary_ = update->summary();
        updateUrl_ = update->downloads;
    } else {
        updateSummary_.clear();
        updateUrl_.clear();
    }
    layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void WelcomeView::layout() {
    items_.clear();
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right;
    int contentWidth = (std::min)(ui::scaled(window_, kContentWidth), width - 2 * ui::scaled(window_, kGutter));
    int left = (width - contentWidth) / 2;
    int rowHeight = ui::scaled(window_, kRowHeight);
    int y = ui::scaled(window_, 26);

    // The heading row is as tall as the icon in it, with the one-line
    // explanation under the pair.
    const int iconSize = ui::scaled(window_, kHeaderIcon);
    headerBounds_ = {left, y, left + contentWidth, y + iconSize};
    y = headerBounds_.bottom + ui::scaled(window_, 6) + ui::scaled(window_, 18) +
        ui::scaled(window_, 14);

    const int bannerHeight = ui::scaled(window_, 66);
    const int buttonWidthInBanner = ui::scaled(window_, 194);
    auto placeBanner = [&](RECT* banner, RECT* button) {
        *banner = {left, y, left + contentWidth, y + bannerHeight};
        *button = {banner->right - buttonWidthInBanner - ui::scaled(window_, 16),
                   banner->top + ui::scaled(window_, 17),
                   banner->right - ui::scaled(window_, 16),
                   banner->top + ui::scaled(window_, 17) + ui::scaled(window_, 32)};
        y += bannerHeight + ui::scaled(window_, 12);
    };

    if (!updateSummary_.empty()) {
        placeBanner(&updateBanner_, &updateButton_);
    } else {
        updateBanner_ = {};
        updateButton_ = {};
    }
    if (showElevationBanner_) {
        placeBanner(&elevationBanner_, &elevateButton_);
    } else {
        elevationBanner_ = {};
        elevateButton_ = {};
    }
    if (!updateSummary_.empty() || showElevationBanner_) y += ui::scaled(window_, 6);

    // Two across rather than one. A single column left most of the window empty
    // and pushed the folder list below the fold; paired rows use the width the
    // window already has.
    const int gap = ui::scaled(window_, 10);
    const int cardWidth = (contentWidth - gap) / 2;

    auto section = [&](const wchar_t* title, const std::vector<ScanSuggestion>& list) {
        if (list.empty()) return;
        Item header;
        header.isHeader = true;
        header.headerText = title;
        header.bounds = {left, y, left + contentWidth, y + ui::scaled(window_, 28)};
        items_.push_back(header);
        y += ui::scaled(window_, 30);
        for (size_t i = 0; i < list.size(); i += 2) {
            for (size_t column = 0; column < 2 && i + column < list.size(); ++column) {
                Item item;
                item.suggestion = list[i + column];
                int cardLeft = left + int(column) * (cardWidth + gap);
                // A lone final entry keeps the width of a paired one rather than
                // stretching across the row.
                item.bounds = {cardLeft, y, cardLeft + cardWidth, y + rowHeight};
                items_.push_back(item);
            }
            y += rowHeight + ui::scaled(window_, 6);
        }
        y += ui::scaled(window_, 14);
    };
    section(L"Drives", volumes_);
    section(L"Folders", folders_);

    int buttonWidth = ui::scaled(window_, 190);
    browseButton_ = {left, y, left + buttonWidth, y + ui::scaled(window_, 34)};
    y += ui::scaled(window_, 34) + ui::scaled(window_, 28);
    contentHeight_ = y;

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    info.nMin = 0;
    info.nMax = (std::max)(0, contentHeight_ - 1);
    info.nPage = UINT(client.bottom);
    scrollY_ = (std::max)(0, (std::min)(scrollY_, (std::max)(0, contentHeight_ - int(client.bottom))));
    info.nPos = scrollY_;
    SetScrollInfo(window_, SB_VERT, &info, TRUE);
}

int WelcomeView::hitTest(int x, int y) const {
    int adjusted = y + scrollY_;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].isHeader) continue;
        const RECT& b = items_[i].bounds;
        if (x >= b.left && x < b.right && adjusted >= b.top && adjusted < b.bottom) return int(i);
    }
    return -1;
}

/// One suggestion: icon, name, and either a description or the drive's capacity
/// with a bar showing how full it is.
void WelcomeView::paintCard(HDC dc, const Item& item, bool hovered, int offset) {
    const ui::Theme& t = ui::theme();
    RECT bounds = item.bounds;
    OffsetRect(&bounds, 0, offset);

    ui::fill(dc, bounds, hovered ? t.hover : t.paneBackground);
    ui::frame(dc, bounds, hovered ? t.selection : t.separator, 1);

    const ScanSuggestion& suggestion = item.suggestion;
    const int pad = ui::scaled(window_, 10);
    const int iconSize = ui::scaled(window_, 32);
    int textLeft = bounds.left + pad;

    if (suggestion.icon) {
        int top = bounds.top + ((bounds.bottom - bounds.top) - iconSize) / 2;
        DrawIconEx(dc, bounds.left + pad, top, suggestion.icon, iconSize, iconSize, 0, nullptr,
                   DI_NORMAL);
        textLeft = bounds.left + pad + iconSize + ui::scaled(window_, 10);
    }

    RECT title{textLeft, bounds.top + ui::scaled(window_, 7), bounds.right - pad,
               bounds.top + ui::scaled(window_, 25)};
    ui::drawText(dc, suggestion.title, title, t.text, ui::font(window_, ui::FontRole::RowTitle),
                 DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    HFONT detailFont = ui::font(window_, ui::FontRole::Small);
    if (suggestion.isVolume && suggestion.capacity > 0) {
        int64_t used = suggestion.capacity - suggestion.free;
        std::wstring detail = ui::formatBytes(used) + L" used of " +
                              ui::formatBytes(suggestion.capacity) + L"  ·  " +
                              ui::formatBytes(suggestion.free) + L" free";
        RECT line{textLeft, title.bottom - ui::scaled(window_, 2), bounds.right - pad,
                  title.bottom + ui::scaled(window_, 14)};
        ui::drawText(dc, detail, line, t.secondaryText, detailFont,
                     DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT track{textLeft, line.bottom + ui::scaled(window_, 3), bounds.right - pad,
                   line.bottom + ui::scaled(window_, 3) + ui::scaled(window_, 6)};
        ui::fill(dc, track, t.dark ? RGB(60, 60, 60) : RGB(226, 226, 230));
        double fraction = double(used) / double(suggestion.capacity);
        fraction = (std::max)(0.01, (std::min)(1.0, fraction));
        RECT filled = track;
        filled.right = track.left + LONG(double(track.right - track.left) * fraction);
        // Red when the drive is nearly full, which is when someone opens this app.
        ui::fill(dc, filled, fraction > 0.9 ? RGB(214, 82, 70) : RGB(72, 140, 208));
    } else {
        std::wstring detail = suggestion.subtitle.empty() ? suggestion.path : suggestion.subtitle;
        RECT line{textLeft, title.bottom - ui::scaled(window_, 1), bounds.right - pad,
                  bounds.bottom - ui::scaled(window_, 6)};
        ui::drawText(dc, detail, line, t.secondaryText, detailFont,
                     DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    }
}

void WelcomeView::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right;
    int height = client.bottom;
    HDC dc = buffer_.begin(target, width, height);
    if (!dc) return;

    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.windowBackground);

    int contentWidth = (std::min)(ui::scaled(window_, kContentWidth), width - 2 * ui::scaled(window_, kGutter));
    int left = (width - contentWidth) / 2;
    int offset = -scrollY_;

    RECT header = headerBounds_;
    OffsetRect(&header, 0, offset);
    int iconSize = ui::scaled(window_, kHeaderIcon);
    int titleLeft = header.left;
    if (appIcon_) {
        DrawIconEx(dc, header.left, header.top, appIcon_, iconSize, iconSize, 0, nullptr,
                   DI_NORMAL);
        titleLeft = header.left + iconSize + ui::scaled(window_, 12);
    }
    RECT title{titleLeft, header.top, left + contentWidth, header.bottom};
    ui::drawText(dc, L"Disk Space", title, t.text, ui::font(window_, ui::FontRole::Large),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT subtitle{header.left, header.bottom + ui::scaled(window_, 4), left + contentWidth,
                  header.bottom + ui::scaled(window_, 24)};
    ui::drawText(dc, L"Choose a drive or folder to scan. Results appear while the scan runs.",
                 subtitle, t.secondaryText, ui::font(window_, ui::FontRole::Body),
                 DT_LEFT | DT_SINGLELINE);

    // Both banners are drawn the same way and differ only in colour and wording,
    // so the shape lives in one place.
    auto drawBanner = [&](const RECT& bannerRect, const RECT& buttonRect, COLORREF fillColour,
                          COLORREF edgeColour, const std::wstring& message,
                          const wchar_t* buttonText, bool buttonHovered) {
        RECT banner = bannerRect;
        OffsetRect(&banner, 0, offset);
        if (banner.bottom < 0 || banner.top > height) return;
        ui::fill(dc, banner, fillColour);
        ui::frame(dc, banner, edgeColour, 1);
        RECT button = buttonRect;
        OffsetRect(&button, 0, offset);
        RECT text{banner.left + ui::scaled(window_, 16), banner.top + ui::scaled(window_, 10),
                  button.left - ui::scaled(window_, 16), banner.bottom - ui::scaled(window_, 8)};
        ui::drawText(dc, message, text, t.text, ui::font(window_, ui::FontRole::Body),
                     DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        ui::fill(dc, button, buttonHovered ? t.hover : t.paneBackground);
        ui::frame(dc, button, t.separator, 1);
        ui::drawText(dc, buttonText, button, t.text, ui::font(window_, ui::FontRole::Body),
                     DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    if (!updateSummary_.empty()) {
        drawBanner(updateBanner_, updateButton_, t.dark ? RGB(24, 44, 58) : RGB(232, 244, 253),
                   t.dark ? RGB(52, 88, 116) : RGB(178, 212, 240),
                   updateSummary_ + L" The scan engine and everything it reports are unchanged "
                                    L"by updating.",
                   L"Get the New Version", hoveringUpdate_);
    }
    if (showElevationBanner_) {
        drawBanner(elevationBanner_, elevateButton_, t.dark ? RGB(48, 44, 24) : RGB(255, 249, 226),
                   t.dark ? RGB(96, 84, 40) : RGB(238, 224, 170),
                   L"Scanning a whole drive is about ten times faster as administrator, because "
                   L"the file table can be read directly. Everything works without it.",
                   L"Restart as Administrator", hoveringElevate_);
    }

    HFONT bodyFont = ui::font(window_, ui::FontRole::Body);
    HFONT smallFont = ui::font(window_, ui::FontRole::Small);

    for (size_t i = 0; i < items_.size(); ++i) {
        RECT bounds = items_[i].bounds;
        OffsetRect(&bounds, 0, offset);
        if (bounds.bottom < 0 || bounds.top > height) continue;

        if (items_[i].isHeader) {
            // Upper case and small, so a heading reads as a label on the group
            // rather than competing with the entries under it.
            std::wstring heading = items_[i].headerText;
            CharUpperBuffW(heading.data(), DWORD(heading.size()));
            RECT label = bounds;
            label.left += ui::scaled(window_, 10);
            ui::drawText(dc, heading, label, t.secondaryText, smallFont,
                         DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            continue;
        }
        paintCard(dc, items_[i], int(i) == hovered_, offset);
    }

    RECT browse = browseButton_;
    OffsetRect(&browse, 0, offset);
    ui::fill(dc, browse, hoveringBrowse_ ? t.hover : t.paneBackground);
    ui::frame(dc, browse, t.separator, 1);
    ui::drawText(dc, L"Choose Another Folder…", browse, t.text, bodyFont,
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    buffer_.blit(target, width, height);
}

LRESULT WelcomeView::handle(UINT message, WPARAM w, LPARAM l) {
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
        case WM_SIZE:
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_ALL;
            GetScrollInfo(window_, SB_VERT, &info);
            switch (LOWORD(w)) {
                case SB_LINEUP: scrollY_ -= ui::scaled(window_, 40); break;
                case SB_LINEDOWN: scrollY_ += ui::scaled(window_, 40); break;
                case SB_PAGEUP: scrollY_ -= int(info.nPage); break;
                case SB_PAGEDOWN: scrollY_ += int(info.nPage); break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: scrollY_ = info.nTrackPos; break;
                default: break;
            }
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEWHEEL:
            scrollY_ -= GET_WHEEL_DELTA_WPARAM(w) * ui::scaled(window_, 60) / WHEEL_DELTA;
            layout();
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            int hovered = hitTest(x, y);
            RECT browse = browseButton_;
            OffsetRect(&browse, 0, -scrollY_);
            bool overBrowse = PtInRect(&browse, POINT{x, y}) != 0;
            RECT elevate = elevateButton_;
            OffsetRect(&elevate, 0, -scrollY_);
            bool overElevate = showElevationBanner_ && PtInRect(&elevate, POINT{x, y}) != 0;
            RECT update = updateButton_;
            OffsetRect(&update, 0, -scrollY_);
            bool overUpdate = !updateSummary_.empty() && PtInRect(&update, POINT{x, y}) != 0;
            if (hovered != hovered_ || overBrowse != hoveringBrowse_ ||
                overElevate != hoveringElevate_ || overUpdate != hoveringUpdate_) {
                hovered_ = hovered;
                hoveringBrowse_ = overBrowse;
                hoveringElevate_ = overElevate;
                hoveringUpdate_ = overUpdate;
                SetCursor(LoadCursorW(
                    nullptr, (hovered >= 0 || overBrowse || overElevate || overUpdate) ? IDC_HAND
                                                                                       : IDC_ARROW));
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window_, 0};
                TrackMouseEvent(&track);
                InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (hovered_ >= 0 || hoveringBrowse_ || hoveringElevate_) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return 1;
            }
            break;
        case WM_MOUSELEAVE:
            hovered_ = -1;
            hoveringBrowse_ = false;
            hoveringElevate_ = false;
            hoveringUpdate_ = false;
            InvalidateRect(window_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
            RECT browse = browseButton_;
            OffsetRect(&browse, 0, -scrollY_);
            if (PtInRect(&browse, POINT{x, y})) {
                if (onBrowse) onBrowse();
                return 0;
            }
            RECT elevate = elevateButton_;
            OffsetRect(&elevate, 0, -scrollY_);
            if (showElevationBanner_ && PtInRect(&elevate, POINT{x, y})) {
                if (onRelaunchElevated) onRelaunchElevated();
                return 0;
            }
            RECT update = updateButton_;
            OffsetRect(&update, 0, -scrollY_);
            if (!updateSummary_.empty() && PtInRect(&update, POINT{x, y})) {
                if (onDownloadUpdate) onDownloadUpdate(updateUrl_);
                return 0;
            }
            int index = hitTest(x, y);
            if (index >= 0 && onPick) onPick(items_[size_t(index)].suggestion.path);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

}  // namespace app

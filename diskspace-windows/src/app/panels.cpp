#include "panels.h"

#include <windowsx.h>

#include <algorithm>

#include "../core/file_kind.h"
#include "../core/win_util.h"

namespace app {
namespace {

constexpr const wchar_t* kInformationClass = L"DiskSpaceInformationPanel";
constexpr const wchar_t* kSelectionClass = L"DiskSpaceSelectionPanel";
constexpr int kRowHeight = 22;

std::wstring formatFileTime(const FILETIME& time) {
    if (time.dwLowDateTime == 0 && time.dwHighDateTime == 0) return L"—";
    SYSTEMTIME utc{}, local{};
    if (!FileTimeToSystemTime(&time, &utc)) return L"—";
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    wchar_t date[64] = {}, clock[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, nullptr, clock, 64);
    return std::wstring(date) + L" " + clock;
}

std::wstring describeAttributes(DWORD attributes) {
    if (attributes == INVALID_FILE_ATTRIBUTES) return L"—";
    std::wstring out;
    auto add = [&](const wchar_t* name) {
        if (!out.empty()) out += L", ";
        out += name;
    };
    if (attributes & FILE_ATTRIBUTE_READONLY) add(L"read only");
    if (attributes & FILE_ATTRIBUTE_HIDDEN) add(L"hidden");
    if (attributes & FILE_ATTRIBUTE_SYSTEM) add(L"system");
    if (attributes & FILE_ATTRIBUTE_COMPRESSED) add(L"compressed");
    if (attributes & FILE_ATTRIBUTE_ENCRYPTED) add(L"encrypted");
    if (attributes & FILE_ATTRIBUTE_SPARSE_FILE) add(L"sparse");
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) add(L"reparse point");
    if (attributes & FILE_ATTRIBUTE_TEMPORARY) add(L"temporary");
    return out.empty() ? L"normal" : out;
}

}  // namespace

// MARK: - Information

InformationPanel& InformationPanel::shared() {
    static InformationPanel instance;
    return instance;
}

LRESULT CALLBACK InformationPanel::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* panel = reinterpret_cast<InformationPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            if (panel) panel->paint(dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, w, l);
}

void InformationPanel::ensureWindow(HWND owner) {
    if (window_) return;
    ui::registerClass(kInformationClass, proc);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kInformationClass, L"Information",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT,
                              CW_USEDEFAULT, 460, 360, owner, nullptr, GetModuleHandleW(nullptr),
                              nullptr);
    SetWindowLongPtrW(window_, GWLP_USERDATA, LONG_PTR(this));
}

void InformationPanel::show(HWND owner) {
    ensureWindow(owner);
    ShowWindow(window_, SW_SHOW);
}

void InformationPanel::hide() {
    if (window_) ShowWindow(window_, SW_HIDE);
}

bool InformationPanel::visible() const { return window_ && IsWindowVisible(window_); }

void InformationPanel::clear() {
    fields_.clear();
    title_.clear();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void InformationPanel::update(const NodeArena& arena, size_t node, const std::wstring& sizeText) {
    if (node >= arena.count()) return;
    std::wstring path = arena.path(node);
    title_ = node == 0 ? path : arena.name(node);

    fields_.clear();
    fields_.emplace_back(L"Kind", diskscan::FileKind::names()[size_t(arena.kind[node])]);
    fields_.emplace_back(L"Size", sizeText);
    fields_.emplace_back(L"On disk", ui::formatBytes(arena.allocated[node]));
    fields_.emplace_back(L"Length", ui::formatBytes(arena.logical[node]));
    if (arena.isDirectory(node)) {
        fields_.emplace_back(L"Contains", ui::formatCount(arena.fileCount[node]) + L" files");
    }

    // One lookup, only when the panel is actually on screen.
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(diskscan::extendedPath(path).c_str(), GetFileExInfoStandard, &data)) {
        fields_.emplace_back(L"Modified", formatFileTime(data.ftLastWriteTime));
        fields_.emplace_back(L"Created", formatFileTime(data.ftCreationTime));
        fields_.emplace_back(L"Attributes", describeAttributes(data.dwFileAttributes));
    } else {
        fields_.emplace_back(L"Modified", L"—");
        fields_.emplace_back(L"Created", L"—");
        fields_.emplace_back(L"Attributes", L"unreadable");
    }
    fields_.emplace_back(L"Path", path);

    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void InformationPanel::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    HDC dc = buffer_.begin(target, client.right, client.bottom);
    if (!dc) return;
    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    int pad = ui::scaled(window_, 16);
    int y = pad;
    RECT title{pad, y, client.right - pad, y + ui::scaled(window_, 26)};
    ui::drawText(dc, title_, title, t.text, ui::font(window_, ui::FontRole::Title),
                 DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    y = title.bottom + ui::scaled(window_, 10);

    HFONT label = ui::font(window_, ui::FontRole::Body);
    int labelWidth = ui::scaled(window_, 92);
    for (const auto& field : fields_) {
        RECT name{pad, y, pad + labelWidth, y + ui::scaled(window_, kRowHeight)};
        ui::drawText(dc, field.first, name, t.secondaryText, label,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        RECT value{name.right + ui::scaled(window_, 10), y, client.right - pad,
                   y + ui::scaled(window_, kRowHeight)};
        // Paths get the ellipsis in the middle, so both the drive and the file
        // name survive; everything else truncates at the end.
        UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
        flags |= field.first == L"Path" ? DT_PATH_ELLIPSIS : DT_END_ELLIPSIS;
        ui::drawText(dc, field.second, value, t.text, label, flags);
        y += ui::scaled(window_, kRowHeight);
    }
    buffer_.blit(target, client.right, client.bottom);
}

// MARK: - Selection

SelectionPanel& SelectionPanel::shared() {
    static SelectionPanel instance;
    return instance;
}

LRESULT CALLBACK SelectionPanel::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    auto* panel = reinterpret_cast<SelectionPanel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            if (panel) panel->paint(dc);
            EndPaint(window, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!panel) break;
            int hovered = panel->rowAt(GET_Y_LPARAM(l));
            if (hovered != panel->hovered_) {
                panel->hovered_ = hovered;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!panel) break;
            int index = panel->rowAt(GET_Y_LPARAM(l));
            if (index >= 0 && panel->onChoose) {
                panel->onChoose(panel->entries_[size_t(index)].node);
            }
            return 0;
        }
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, w, l);
}

void SelectionPanel::ensureWindow(HWND owner) {
    if (window_) return;
    ui::registerClass(kSelectionClass, proc);
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, kSelectionClass, L"Selection",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_SIZEBOX, CW_USEDEFAULT,
                              CW_USEDEFAULT, 440, 320, owner, nullptr, GetModuleHandleW(nullptr),
                              nullptr);
    SetWindowLongPtrW(window_, GWLP_USERDATA, LONG_PTR(this));
}

void SelectionPanel::show(HWND owner) {
    ensureWindow(owner);
    ShowWindow(window_, SW_SHOW);
}

void SelectionPanel::hide() {
    if (window_) ShowWindow(window_, SW_HIDE);
}

bool SelectionPanel::visible() const { return window_ && IsWindowVisible(window_); }

void SelectionPanel::clear() {
    entries_.clear();
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void SelectionPanel::update(const NodeArena& arena, int32_t node,
                            const std::function<std::wstring(size_t)>& sizeText) {
    entries_.clear();
    if (node < 0 || size_t(uint32_t(node)) >= arena.count()) return;
    std::vector<int32_t> chain;
    size_t cur = size_t(uint32_t(node));
    while (true) {
        chain.push_back(int32_t(cur));
        if (cur == 0) break;
        cur = size_t(uint32_t(arena.parent[cur]));
    }
    int depth = 0;
    for (size_t i = chain.size(); i-- > 0;) {
        size_t index = size_t(uint32_t(chain[i]));
        entries_.push_back({chain[i], index == 0 ? arena.path(0) : arena.name(index),
                            sizeText(index), depth++});
    }
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

int SelectionPanel::rowAt(int y) const {
    int index = y / ui::scaled(window_, kRowHeight);
    return index >= 0 && index < int(entries_.size()) ? index : -1;
}

void SelectionPanel::paint(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    HDC dc = buffer_.begin(target, client.right, client.bottom);
    if (!dc) return;
    const ui::Theme& t = ui::theme();
    ui::fill(dc, client, t.paneBackground);

    HFONT font = ui::font(window_, ui::FontRole::Body);
    int rowHeight = ui::scaled(window_, kRowHeight);
    int sizeWidth = ui::scaled(window_, 110);
    int indent = ui::scaled(window_, 12);
    for (size_t i = 0; i < entries_.size(); ++i) {
        RECT bounds{0, int(i) * rowHeight, client.right, int(i + 1) * rowHeight};
        if (int(i) == hovered_) ui::fill(dc, bounds, t.hover);
        RECT name{ui::scaled(window_, 10) + entries_[i].depth * indent, bounds.top,
                  client.right - sizeWidth, bounds.bottom};
        ui::drawText(dc, entries_[i].name, name, t.text, font,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        RECT size{client.right - sizeWidth, bounds.top, client.right - ui::scaled(window_, 10),
                  bounds.bottom};
        ui::drawText(dc, entries_[i].size, size, t.secondaryText, font,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    buffer_.blit(target, client.right, client.bottom);
}

}  // namespace app

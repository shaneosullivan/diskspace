#include "ui_common.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <cstdio>
#include <algorithm>
#include <map>
#include <unordered_set>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace ui {
namespace {

Theme gTheme;
bool gThemeLoaded = false;

bool systemPrefersDark() {
    // An override for checking both themes without changing the machine's
    // personalisation setting, which is the user's and not ours to flip.
    wchar_t forced[16] = {};
    if (GetEnvironmentVariableW(L"DISKSPACE_THEME", forced, 16) > 0) {
        if (_wcsicmp(forced, L"dark") == 0) return true;
        if (_wcsicmp(forced, L"light") == 0) return false;
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    bool dark = false;
    if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        dark = value == 0;
    }
    RegCloseKey(key);
    return dark;
}

void loadTheme() {
    const bool dark = systemPrefersDark();
    gTheme.dark = dark;
    if (dark) {
        gTheme.windowBackground = RGB(32, 32, 32);
        gTheme.paneBackground = RGB(24, 24, 24);
        gTheme.alternateRow = RGB(30, 30, 30);
        gTheme.text = RGB(240, 240, 240);
        gTheme.secondaryText = RGB(160, 160, 160);
        gTheme.separator = RGB(60, 60, 60);
        gTheme.headerBackground = RGB(40, 40, 40);
        gTheme.headerText = RGB(200, 200, 200);
        gTheme.selection = RGB(0, 90, 158);
        gTheme.selectionText = RGB(255, 255, 255);
        gTheme.hover = RGB(48, 48, 48);
    } else {
        gTheme.windowBackground = RGB(243, 243, 243);
        gTheme.paneBackground = RGB(255, 255, 255);
        gTheme.alternateRow = RGB(246, 246, 248);
        gTheme.text = RGB(28, 28, 30);
        gTheme.secondaryText = RGB(110, 110, 115);
        gTheme.separator = RGB(216, 216, 220);
        gTheme.headerBackground = RGB(250, 250, 250);
        gTheme.headerText = RGB(80, 80, 84);
        gTheme.selection = RGB(0, 120, 215);
        gTheme.selectionText = RGB(255, 255, 255);
        gTheme.hover = RGB(236, 240, 246);
    }
    // Dark in both themes on purpose: the map is a field of saturated colour, and
    // a light surround washes every cushion out.
    gTheme.treemapBackground = RGB(28, 28, 30);
    gThemeLoaded = true;
}

std::map<std::pair<int, int>, HFONT>& fontCache() {
    static std::map<std::pair<int, int>, HFONT> cache;
    return cache;
}

/// One brush per colour, reused. Creating and destroying one per fill shows up
/// in a profile the moment a list is scrolled quickly.
HBRUSH brushFor(COLORREF colour) {
    static std::map<COLORREF, HBRUSH> cache;
    auto it = cache.find(colour);
    if (it != cache.end()) return it->second;
    HBRUSH brush = CreateSolidBrush(colour);
    cache[colour] = brush;
    return brush;
}

}  // namespace

const Theme& theme() {
    if (!gThemeLoaded) loadTheme();
    return gTheme;
}

bool refreshTheme() {
    bool was = gTheme.dark;
    loadTheme();
    return was != gTheme.dark;
}

int dpiOf(HWND window) {
    UINT dpi = window ? GetDpiForWindow(window) : 96;
    return dpi ? int(dpi) : 96;
}

int scaled(HWND window, int value) { return MulDiv(value, dpiOf(window), 96); }

HFONT font(HWND window, FontRole role) {
    int points = 9;
    int weight = FW_NORMAL;
    switch (role) {
        case FontRole::Small: points = 8; break;
        case FontRole::Header: points = 9; break;
        case FontRole::RowTitle: points = 10; weight = FW_SEMIBOLD; break;
        case FontRole::Title: points = 12; weight = FW_SEMIBOLD; break;
        case FontRole::Large: points = 16; weight = FW_SEMIBOLD; break;
        default: break;
    }
    int dpi = dpiOf(window);
    auto key = std::make_pair(points * 1000 + weight, dpi);
    auto& cache = fontCache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(points, dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    HFONT created = CreateFontIndirectW(&lf);
    cache[key] = created;
    return created;
}

std::wstring formatBytes(int64_t bytes) {
    static const wchar_t* units[] = {L"B", L"kB", L"MB", L"GB", L"TB", L"PB"};
    // Decimal rather than binary units, to agree with what the drive is sold as
    // and with what File Explorer's capacity figures mean.
    double value = double(bytes);
    int unit = 0;
    while ((value >= 1000 || value <= -1000) && unit < 5) {
        value /= 1000;
        ++unit;
    }
    wchar_t buffer[64];
    if (unit == 0) {
        swprintf_s(buffer, L"%lld B", (long long)bytes);
    } else {
        swprintf_s(buffer, L"%.1f %s", value, units[unit]);
    }
    return buffer;
}

std::wstring formatCount(int64_t count) {
    std::wstring digits = std::to_wstring(count < 0 ? -count : count);
    std::wstring out;
    int group = 0;
    for (size_t i = digits.size(); i-- > 0;) {
        out.push_back(digits[i]);
        if (++group % 3 == 0 && i > 0) out.push_back(L',');
    }
    if (count < 0) out.push_back(L'-');
    std::reverse(out.begin(), out.end());
    return out;
}

std::wstring formatSeconds(double seconds, bool precise) {
    wchar_t buffer[32];
    swprintf_s(buffer, precise ? L"%.2fs" : L"%.1fs", seconds);
    return buffer;
}

void fill(HDC dc, const RECT& rect, COLORREF colour) {
    FillRect(dc, &rect, brushFor(colour));
}

void frame(HDC dc, const RECT& rect, COLORREF colour, int thickness) {
    HBRUSH brush = brushFor(colour);
    RECT edge;
    edge = {rect.left, rect.top, rect.right, rect.top + thickness};
    FillRect(dc, &edge, brush);
    edge = {rect.left, rect.bottom - thickness, rect.right, rect.bottom};
    FillRect(dc, &edge, brush);
    edge = {rect.left, rect.top, rect.left + thickness, rect.bottom};
    FillRect(dc, &edge, brush);
    edge = {rect.right - thickness, rect.top, rect.right, rect.bottom};
    FillRect(dc, &edge, brush);
}

void drawText(HDC dc, const std::wstring& text, const RECT& rect, COLORREF colour, HFONT font,
              UINT flags) {
    if (text.empty()) return;
    HFONT previous = HFONT(SelectObject(dc, font));
    int previousMode = SetBkMode(dc, TRANSPARENT);
    COLORREF previousColour = SetTextColor(dc, colour);
    RECT box = rect;
    DrawTextW(dc, text.c_str(), int(text.size()), &box, flags);
    SetTextColor(dc, previousColour);
    SetBkMode(dc, previousMode);
    SelectObject(dc, previous);
}

SIZE measureText(HDC dc, const std::wstring& text, HFONT font) {
    HFONT previous = HFONT(SelectObject(dc, font));
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), int(text.size()), &size);
    SelectObject(dc, previous);
    return size;
}

BackBuffer::~BackBuffer() {
    if (dc_) {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
    }
}

HDC BackBuffer::begin(HDC target, int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    if (dc_ && (width != width_ || height != height_)) {
        SelectObject(dc_, previous_);
        DeleteObject(bitmap_);
        DeleteDC(dc_);
        dc_ = nullptr;
    }
    if (!dc_) {
        dc_ = CreateCompatibleDC(target);
        bitmap_ = CreateCompatibleBitmap(target, width, height);
        previous_ = HBITMAP(SelectObject(dc_, bitmap_));
        width_ = width;
        height_ = height;
    }
    return dc_;
}

void BackBuffer::blit(HDC target, int width, int height) {
    if (dc_) BitBlt(target, 0, 0, width, height, dc_, 0, 0, SRCCOPY);
}

void initialiseAppTheme() {
    // Ordinal 135 of uxtheme is SetPreferredAppMode, which is what makes the
    // common controls and scrollbars honour a dark palette. It has never been
    // given a public name, so it is looked up by ordinal and simply skipped when
    // it is not there — the only consequence is light scrollbars.
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    auto setPreferredAppMode =
        reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(135)));
    if (setPreferredAppMode) setPreferredAppMode(PreferredAppMode::AllowDark);
}

void applySystemTheme(HWND window) {
    if (!window) return;
    BOOL dark = theme().dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE. Documented from Windows 10 2004; on anything
    // older the call fails harmlessly and the title bar stays light.
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    SetWindowTheme(window, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void drawOwnerDrawnButton(const DRAWITEMSTRUCT& item, const std::wstring& text, HFONT font) {
    const Theme& t = theme();
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;

    COLORREF face = pressed ? t.separator : (t.dark ? RGB(45, 45, 45) : RGB(252, 252, 252));
    fill(item.hDC, item.rcItem, face);
    frame(item.hDC, item.rcItem, focused ? t.selection : t.separator, focused ? 2 : 1);
    drawText(item.hDC, text, item.rcItem, disabled ? t.secondaryText : t.text, font,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void registerClass(const wchar_t* name, WNDPROC proc, HCURSOR cursor) {
    static std::unordered_set<std::wstring> registered;
    if (registered.count(name)) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = cursor ? cursor : LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // every pane paints its own background
    wc.lpszClassName = name;
    RegisterClassExW(&wc);
    registered.insert(name);
}

}  // namespace ui

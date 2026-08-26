#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace ui {

/// Colours for the whole application, resolved once from the system theme.
///
/// Windows exposes no palette for a custom-drawn control the way AppKit does, so
/// these are named the way they are used rather than after a system colour, and
/// the light and dark sets are chosen together.
struct Theme {
    bool dark = false;
    COLORREF windowBackground;
    COLORREF paneBackground;
    COLORREF alternateRow;
    COLORREF text;
    COLORREF secondaryText;
    COLORREF separator;
    COLORREF headerBackground;
    COLORREF headerText;
    COLORREF selection;
    COLORREF selectionText;
    COLORREF hover;
    /// The treemap's own backdrop, which stays dark in both themes: the map is a
    /// field of saturated colour and a light surround washes it out.
    COLORREF treemapBackground;
};

const Theme& theme();
/// Re-reads the system light/dark setting. Returns true when it changed.
bool refreshTheme();

/// Scales a design-time measurement, which is always quoted at 96 dpi.
int scaled(HWND window, int value);
int dpiOf(HWND window);

enum class FontRole { Body, Small, Header, RowTitle, Title, Large };
/// Cached per size; never destroyed, because they live as long as the process.
HFONT font(HWND window, FontRole role);

std::wstring formatBytes(int64_t bytes);
std::wstring formatCount(int64_t count);
/// "3.2s" while a scan runs, "3.24s" once it has finished.
std::wstring formatSeconds(double seconds, bool precise);

/// Fills a rectangle with a solid colour without allocating a brush per call.
void fill(HDC dc, const RECT& rect, COLORREF colour);
void frame(HDC dc, const RECT& rect, COLORREF colour, int thickness = 1);

/// Draws text clipped to `rect`, truncating with an ellipsis where it does not
/// fit. `flags` takes the usual DT_* values.
void drawText(HDC dc, const std::wstring& text, const RECT& rect, COLORREF colour, HFONT font,
              UINT flags);
SIZE measureText(HDC dc, const std::wstring& text, HFONT font);

/// A memory bitmap the size of the window, so a custom-drawn pane can compose off
/// screen and blit once. Repainting a list row by row straight to the window
/// flickers badly at any real scroll speed.
class BackBuffer {
public:
    ~BackBuffer();
    /// Returns a DC to draw into, resizing the bitmap if the size changed.
    HDC begin(HDC target, int width, int height);
    void blit(HDC target, int width, int height);

private:
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HBITMAP previous_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

/// Registers a window class once, keyed by name.
void registerClass(const wchar_t* name, WNDPROC proc, HCURSOR cursor = nullptr);

/// Tells the system parts of a window to follow the current theme: the title bar
/// through DWM, and the non-client scrollbars through the theme name.
///
/// Everything this application draws itself already honours the theme. These are
/// the pieces Windows draws on our behalf, and left alone they stay light inside
/// a dark window.
void applySystemTheme(HWND window);

/// Called once at startup, before any window exists, so the common controls pick
/// up the right palette. Best effort: the entry point it needs is not documented,
/// and its absence simply leaves the controls light.
void initialiseAppTheme();

/// Paints a push button in the application's own colours, for the toolbar
/// controls that would otherwise be the only light rectangles in a dark window.
void drawOwnerDrawnButton(const DRAWITEMSTRUCT& item, const std::wstring& text, HFONT font);

}  // namespace ui

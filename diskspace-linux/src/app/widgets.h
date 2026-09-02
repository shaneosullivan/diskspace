#pragma once

#include <functional>
#include <string>
#include <vector>

#include "theme.h"
#include "x11.h"

namespace app {

/// Where the pointer is and whether it is held down.
///
/// Drawn controls have no toolkit keeping track of this for them, so one struct
/// is passed to every draw call and every control works out its own hover and
/// pressed state from it. Cheaper than it sounds: there are perhaps twenty
/// controls, and the test is a rectangle.
struct Pointer {
    int x = -1000;
    int y = -1000;
    bool down = false;
    /// Which modifiers were held at the moment of the last press. Only meaningful
    /// while handling one, which is the only time anything asks.
    bool shift = false;
    bool control = false;
};

/// A push button, or a toggle when `toggled` is used.
struct Button {
    ui::Rect bounds;
    std::string label;
    bool enabled = true;
    bool toggled = false;
    /// Drawn with no border or fill until the pointer is over it. For the header,
    /// where a row of framed buttons is heavier than the content beneath them.
    bool flat = false;
    std::function<void()> onClick;

    bool hit(int x, int y) const { return enabled && bounds.contains(x, y); }
    void draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer,
              const ui::Font& font) const;
    /// The width this button wants for its label, plus padding.
    int preferredWidth(ui::Canvas& canvas, const ui::Font& font) const;
};

/// A vertical scrollbar that owns its own drag.
class Scrollbar {
public:
    /// `track` is where the bar is drawn; the content and viewport heights decide
    /// whether it is needed and how big the thumb is.
    void setGeometry(const ui::Rect& track, int contentHeight, int viewportHeight);

    bool visible() const { return contentHeight_ > viewportHeight_ && viewportHeight_ > 0; }
    int offset() const { return offset_; }
    int maxOffset() const;
    void setOffset(int value);
    void scrollBy(int delta) { setOffset(offset_ + delta); }
    /// Bring the band from `top` to `top + height` into view, moving as little as
    /// possible.
    void reveal(int top, int height);

    void draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer) const;

    /// True when the press was the scrollbar's, and it has taken the drag.
    bool press(int x, int y);
    /// True when a drag is in progress and the offset changed.
    bool drag(int y);
    void release() { dragging_ = false; }
    bool dragging() const { return dragging_; }

    static constexpr int kWidth = 10;

private:
    ui::Rect thumb() const;

    ui::Rect track_;
    int contentHeight_ = 0;
    int viewportHeight_ = 0;
    int offset_ = 0;
    bool dragging_ = false;
    /// Where in the thumb the drag started, so it does not jump under the cursor.
    int grabOffset_ = 0;
};

/// One entry in a popup menu. An empty label is a separator.
struct MenuItem {
    std::string label;
    bool enabled = true;
    bool checked = false;
    bool checkable = false;
    std::function<void()> action;
};

/// A menu drawn inside the main window rather than in a window of its own.
///
/// An X popup means an override-redirect window and a pointer grab, and gets the
/// keyboard focus rules wrong in a dozen small ways that a toolkit would
/// otherwise be handling. Drawn as an overlay it is a rectangle and a list, it
/// cannot get lost behind anything, and the only thing given up is the ability
/// to extend past the window's edge - so it is clamped inside instead.
class Menu {
public:
    void open(const std::vector<MenuItem>& items, int x, int y);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    /// Where it will be drawn, once the size is known.
    void layout(ui::Canvas& canvas, const ui::Font& font);
    void draw(ui::Canvas& canvas, const Theme& theme, const Pointer& pointer,
              const ui::Font& font) const;

    /// Handle a click. Returns true when the menu consumed it, which includes a
    /// click outside that merely dismisses it.
    bool click(int x, int y);
    bool contains(int x, int y) const { return open_ && bounds_.contains(x, y); }

private:
    std::vector<MenuItem> items_;
    ui::Rect bounds_;
    int wantX_ = 0;
    int wantY_ = 0;
    bool open_ = false;
    bool measured_ = false;

    static constexpr int kItemHeight = 26;
    static constexpr int kSeparatorHeight = 9;
    static constexpr int kPadding = 6;
};

/// Common metrics, scaled once so every pane agrees about them.
struct Metrics {
    int rowHeight = 22;
    int headerHeight = 44;
    int statusHeight = 26;
    int padding = 8;
    int indent = 16;
    int sizeColumn = 110;

    static Metrics forScale(double scale);
};

/// The sizes anything here draws in.
struct Fonts {
    ui::Font regular;
    ui::Font bold;
    ui::Font small;
    ui::Font title;
    /// The welcome screen's one line of large text, and nothing else.
    ui::Font heading;

    static Fonts load(ui::Display* display);
};

/// Everything a pane needs in order to draw itself.
///
/// Passed by reference to every draw and every hit test, which is the whole of
/// the arrangement that replaced a widget hierarchy. A pane owns its bounds and
/// its own scroll position and nothing else.
struct Context {
    Theme theme;
    Metrics metrics;
    Fonts fonts;
    Pointer pointer;
};

}  // namespace app

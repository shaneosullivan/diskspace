#pragma once

#include <functional>
#include <string>
#include <vector>

#include "widgets.h"
#include "x11.h"

namespace app {

/// The settings, as an overlay on the main window.
///
/// A panel rather than a second window, for the same reason the menus are: a
/// dialog in X means another top-level, its own transient-for hint, its own
/// focus handling and its own event loop reentry, and none of that buys anything
/// for a list of eight checkboxes.
class PreferencesPanel {
public:
    void open() { open_ = true; }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    void draw(ui::Canvas& canvas, const Context& context);
    bool mousePress(int x, int y, int button, const Context& context);
    bool key(unsigned long keysym);

    std::function<void()> onNeedsRedraw;

private:
    struct Row {
        std::string label;
        std::string detail;
        bool (*get)();
        void (*set)(bool);
    };

    static const std::vector<Row>& rows();
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    bool open_ = false;
    ui::Rect panel_;
    Button closeButton_;
    Button resetButton_;
    std::vector<ui::Rect> rowBounds_;
};

}  // namespace app

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "widgets.h"
#include "x11.h"

namespace app {

/// "Choose a folder to scan", drawn here.
///
/// GtkFileDialog was the one piece of the toolkit with nothing behind it to
/// fall back on: keeping it needs either a widget library or the desktop's file
/// portal, and the portal is a running service this is meant not to require.
///
/// So it is a list of directories with a path bar, which is all this ever needed
/// - the answer is always a folder, never a file, and there is no filtering, no
/// preview and nothing to save. It draws with the same list code as the panes and
/// opens as an overlay on the main window rather than in a window of its own.
class FolderChooser {
public:
    void open(const std::string& startPath);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    void draw(ui::Canvas& canvas, const Context& context);

    bool mousePress(int x, int y, int button, int clicks, const Context& context);
    bool mouseDrag(int x, int y);
    void mouseRelease() { scrollbar_.release(); }
    bool scroll(int x, int y, int steps, const Context& context);
    bool key(const ui::KeyEvent& event, const Context& context);

    std::function<void(const std::string&)> onChosen;
    std::function<void()> onNeedsRedraw;

private:
    struct Entry {
        std::string name;
        bool readable = true;
    };

    void navigateTo(const std::string& path);
    void goUp();
    ui::Rect panelIn(const ui::Canvas& canvas, const Context& context) const;
    void redraw() {
        if (onNeedsRedraw) onNeedsRedraw();
    }

    bool open_ = false;
    std::string path_;
    std::vector<Entry> entries_;
    int selected_ = -1;
    /// Whether the directory could be read at all, so an unreadable folder says
    /// so rather than looking empty.
    bool readable_ = true;

    Scrollbar scrollbar_;
    Button chooseButton_;
    Button cancelButton_;
    Button upButton_;
    /// Places worth one click: home, the filesystem root, and any mounted volume.
    std::vector<std::pair<std::string, std::string>> shortcuts_;
    ui::Rect panel_;
    ui::Rect listBounds_;
};

}  // namespace app

#include "folder_chooser.h"

#include <X11/keysym.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>

#include "format.h"
#include "platform.h"

namespace app {
namespace {

constexpr int kSidebarWidth = 170;

std::string parentOf(const std::string& path) {
    if (path == "/" || path.empty()) return "/";
    const size_t slash = path.find_last_of('/');
    if (slash == 0) return "/";
    if (slash == std::string::npos) return "/";
    return path.substr(0, slash);
}

std::string join(const std::string& directory, const std::string& name) {
    if (directory == "/") return "/" + name;
    return directory + "/" + name;
}

}  // namespace

void FolderChooser::open(const std::string& startPath) {
    open_ = true;
    shortcuts_.clear();
    if (const char* home = ::getenv("HOME"); home && *home) {
        shortcuts_.emplace_back("Home", home);
    }
    shortcuts_.emplace_back("Filesystem", "/");
    // Whatever the chooser on the welcome screen would offer, so the two agree
    // about what counts as a place worth going.
    for (const diskscan::VolumeInfo& volume : diskscan::mountedVolumes()) {
        if (volume.mountPoint == "/" ) continue;
        shortcuts_.emplace_back(volume.mountPoint, volume.mountPoint);
    }

    std::string start = startPath;
    struct stat info {};
    if (start.empty() || ::stat(start.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) {
        const char* home = ::getenv("HOME");
        start = home && *home ? home : "/";
    }
    navigateTo(start);
}

void FolderChooser::navigateTo(const std::string& path) {
    path_ = path;
    entries_.clear();
    selected_ = -1;
    scrollbar_.setOffset(0);
    readable_ = true;

    DIR* directory = ::opendir(path.c_str());
    if (!directory) {
        readable_ = false;
        redraw();
        return;
    }
    while (const struct dirent* entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        // Hidden folders are not offered. Anyone who wants one can type the path,
        // and a home directory is otherwise mostly dot-directories.
        if (!name.empty() && name[0] == '.') continue;

        // d_type is unknown on some filesystems, so it is a hint rather than an
        // answer: stat settles it when the readdir entry will not.
        bool isDirectory = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            struct stat info {};
            isDirectory = ::stat(join(path, name).c_str(), &info) == 0 && S_ISDIR(info.st_mode);
        }
        if (!isDirectory) continue;

        Entry item;
        item.name = name;
        item.readable = ::access(join(path, name).c_str(), R_OK | X_OK) == 0;
        entries_.push_back(item);
    }
    ::closedir(directory);

    std::sort(entries_.begin(), entries_.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });
    redraw();
}

void FolderChooser::goUp() {
    if (path_ == "/") return;
    const std::string child = path_;
    navigateTo(parentOf(path_));
    // The folder just left is selected, so going up and down again is not a hunt
    // for where you were.
    const std::string name = child.substr(child.find_last_of('/') + 1);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            selected_ = int(i);
            break;
        }
    }
}

ui::Rect FolderChooser::panelIn(const ui::Canvas& canvas, const Context& context) const {
    const int width = std::min(canvas.width() - 60, context.metrics.rowHeight * 34);
    const int height = std::min(canvas.height() - 60, context.metrics.rowHeight * 24);
    return ui::Rect{(canvas.width() - width) / 2, (canvas.height() - height) / 2, width, height};
}

void FolderChooser::draw(ui::Canvas& canvas, const Context& context) {
    if (!open_) return;
    const Theme& theme = context.theme;

    // Everything behind is dimmed, which is what says the rest of the window is
    // not listening at the moment.
    canvas.fill(ui::Rect{0, 0, canvas.width(), canvas.height()}, ui::rgba(0, 0, 0, 0x66));

    panel_ = panelIn(canvas, context);
    canvas.fillRounded(panel_, 10, theme.background);
    canvas.frame(panel_, theme.border);

    const int rowHeight = context.metrics.rowHeight;
    const int padding = context.metrics.padding;
    int y = panel_.y + padding;

    canvas.textIn(ui::Rect{panel_.x + padding + 4, y, panel_.width - 2 * padding, rowHeight + 6},
                  "Choose a folder to scan", context.fonts.title, theme.text);
    y += rowHeight + 10;

    // The path bar, with an "up" button on its left.
    upButton_.bounds = ui::Rect{panel_.x + padding, y, rowHeight + 8, rowHeight + 4};
    upButton_.label = "\xe2\x86\x91";
    upButton_.enabled = path_ != "/";
    upButton_.draw(canvas, theme, context.pointer, context.fonts.regular);

    const ui::Rect pathBar{upButton_.bounds.right() + 6, y,
                           panel_.width - 2 * padding - upButton_.bounds.width - 6, rowHeight + 4};
    canvas.fillRounded(pathBar, 5, theme.surface);
    canvas.frame(pathBar, theme.border);
    canvas.textIn(pathBar.inset(6), displayName(path_.data(), path_.size()), context.fonts.regular,
                  theme.text);
    y += rowHeight + 12;

    const int listTop = y;
    const int buttonRow = panel_.bottom() - padding - rowHeight - 6;
    const int listHeight = buttonRow - listTop - 10;

    // The sidebar of places.
    const ui::Rect sidebar{panel_.x + padding, listTop, kSidebarWidth, listHeight};
    canvas.fillRounded(sidebar, 6, theme.surface);
    for (size_t i = 0; i < shortcuts_.size(); ++i) {
        const ui::Rect row{sidebar.x, sidebar.y + int(i) * rowHeight, sidebar.width, rowHeight};
        if (row.bottom() > sidebar.bottom()) break;
        const bool hovered = row.contains(context.pointer.x, context.pointer.y);
        if (hovered) canvas.fillRounded(row, 4, theme.surfaceHover);
        canvas.textIn(ui::Rect{row.x + 10, row.y, row.width - 16, row.height}, shortcuts_[i].first,
                      context.fonts.regular, theme.text);
    }

    // The folder list.
    listBounds_ = ui::Rect{sidebar.right() + 8, listTop,
                           panel_.right() - padding - sidebar.right() - 8, listHeight};
    canvas.fillRounded(listBounds_, 6, theme.well);
    canvas.frame(listBounds_, theme.border);

    scrollbar_.setGeometry(ui::Rect{listBounds_.right() - Scrollbar::kWidth - 1, listBounds_.y + 1,
                                    Scrollbar::kWidth, listBounds_.height - 2},
                           int(entries_.size()) * rowHeight, listBounds_.height);

    canvas.pushClip(listBounds_.inset(1));
    if (!readable_) {
        canvas.textIn(listBounds_.inset(12), "This folder cannot be read.", context.fonts.regular,
                      theme.dimText);
    } else if (entries_.empty()) {
        canvas.textIn(listBounds_.inset(12), "No folders here.", context.fonts.regular,
                      theme.dimText);
    }

    const int offset = scrollbar_.offset();
    const int first = std::max(0, offset / rowHeight);
    const int last = std::min(int(entries_.size()), (offset + listBounds_.height) / rowHeight + 2);
    for (int i = first; i < last; ++i) {
        const Entry& entry = entries_[size_t(i)];
        const ui::Rect row{listBounds_.x + 1, listBounds_.y + i * rowHeight - offset,
                           listBounds_.width - 2, rowHeight};
        if (i == selected_) {
            canvas.fill(row, theme.selection);
        } else if (row.contains(context.pointer.x, context.pointer.y)) {
            canvas.fill(row, theme.dark ? ui::rgba(0xff, 0xff, 0xff, 0x0e)
                                        : ui::rgba(0, 0, 0, 0x0a));
        }
        const ui::Color colour = i == selected_ ? theme.selectionText
                                 : entry.readable ? theme.text
                                                  : theme.dimText;
        // A folder mark, so a list of names reads as a list of folders.
        canvas.fill(ui::Rect{row.x + 10, row.y + rowHeight / 2 - 5, 12, 9},
                    i == selected_ ? theme.selectionText : theme.accent);
        canvas.textIn(ui::Rect{row.x + 30, row.y, row.width - 40, rowHeight},
                      displayName(entry.name.data(), entry.name.size()), context.fonts.regular,
                      colour);
    }
    canvas.popClip();
    scrollbar_.draw(canvas, theme, context.pointer);

    // The buttons.
    cancelButton_.label = "Cancel";
    chooseButton_.label = "Scan This Folder";
    const int chooseWidth = chooseButton_.preferredWidth(canvas, context.fonts.regular) + 16;
    const int cancelWidth = cancelButton_.preferredWidth(canvas, context.fonts.regular) + 16;
    chooseButton_.bounds = ui::Rect{panel_.right() - padding - chooseWidth, buttonRow, chooseWidth,
                                    rowHeight + 6};
    cancelButton_.bounds = ui::Rect{chooseButton_.bounds.x - cancelWidth - 8, buttonRow,
                                    cancelWidth, rowHeight + 6};
    cancelButton_.draw(canvas, theme, context.pointer, context.fonts.regular);
    chooseButton_.draw(canvas, theme, context.pointer, context.fonts.regular);
}

bool FolderChooser::mousePress(int x, int y, int button, int clicks, const Context& context) {
    if (!open_) return false;
    if (button != 1) return true;

    if (!panel_.contains(x, y)) {
        // A click outside dismisses, the same as Cancel.
        open_ = false;
        redraw();
        return true;
    }
    if (scrollbar_.press(x, y)) {
        redraw();
        return true;
    }
    if (cancelButton_.hit(x, y)) {
        open_ = false;
        redraw();
        return true;
    }
    if (chooseButton_.hit(x, y)) {
        const std::string chosen =
            selected_ >= 0 ? join(path_, entries_[size_t(selected_)].name) : path_;
        open_ = false;
        if (onChosen) onChosen(chosen);
        return true;
    }
    if (upButton_.hit(x, y)) {
        goUp();
        redraw();
        return true;
    }

    // The sidebar.
    const int rowHeight = context.metrics.rowHeight;
    const ui::Rect sidebar{panel_.x + context.metrics.padding, listBounds_.y, kSidebarWidth,
                           listBounds_.height};
    if (sidebar.contains(x, y)) {
        const size_t index = size_t((y - sidebar.y) / rowHeight);
        if (index < shortcuts_.size()) navigateTo(shortcuts_[index].second);
        return true;
    }

    if (listBounds_.contains(x, y)) {
        const int index = (y - listBounds_.y + scrollbar_.offset()) / rowHeight;
        if (index < 0 || size_t(index) >= entries_.size()) return true;
        if (clicks >= 2) {
            navigateTo(join(path_, entries_[size_t(index)].name));
        } else {
            selected_ = index;
            redraw();
        }
        return true;
    }
    return true;
}

bool FolderChooser::mouseDrag(int x, int y) {
    if (!open_ || !scrollbar_.dragging()) return open_;
    if (scrollbar_.drag(y)) redraw();
    return true;
}

bool FolderChooser::scroll(int x, int y, int steps, const Context& context) {
    if (!open_) return false;
    if (listBounds_.contains(x, y)) {
        scrollbar_.scrollBy(steps * context.metrics.rowHeight * 3);
        redraw();
    }
    return true;
}

bool FolderChooser::key(const ui::KeyEvent& event, const Context& context) {
    if (!open_) return false;
    switch (event.key) {
        case XK_Escape:
            open_ = false;
            redraw();
            return true;
        case XK_BackSpace:
            goUp();
            redraw();
            return true;
        case XK_Up:
        case XK_Down: {
            if (entries_.empty()) return true;
            const int next = std::clamp(selected_ + (event.key == XK_Down ? 1 : -1), 0,
                                        int(entries_.size()) - 1);
            selected_ = next;
            scrollbar_.reveal(next * context.metrics.rowHeight, context.metrics.rowHeight);
            redraw();
            return true;
        }
        case XK_Right:
            if (selected_ >= 0 && size_t(selected_) < entries_.size()) {
                navigateTo(join(path_, entries_[size_t(selected_)].name));
            }
            return true;
        case XK_Left:
            goUp();
            redraw();
            return true;
        case XK_Return: {
            const std::string chosen =
                selected_ >= 0 ? join(path_, entries_[size_t(selected_)].name) : path_;
            open_ = false;
            if (onChosen) onChosen(chosen);
            return true;
        }
        default:
            return true;  // the dialog is modal: nothing behind it hears anything
    }
}

}  // namespace app

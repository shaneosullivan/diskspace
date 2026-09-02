#include "window.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cstdio>

#include "file_kind.h"
#include "format.h"
#include "preferences.h"
#include "product.h"
#include "shell.h"
#include "update_check.h"

namespace app {
namespace {

using diskscan::NameString;
using diskscan::SizeMetric;

const char* const kMetricNames[] = {
    "Current size",
    "Added in the last month",
    "Added in the last six months",
    "Added since last scan",
};

/// Ids for the two blocks drawn alongside the scan. Negative, so they cannot
/// collide with a node index.
constexpr int32_t kFreeSpaceId = -2;
constexpr int32_t kOtherSpaceId = -3;

constexpr int kSplitterWidth = 6;

}  // namespace

MainWindow::MainWindow(ui::Display* display) : display_(display), window_(display) {}
MainWindow::~MainWindow() = default;

bool MainWindow::create() {
    context_.theme = Theme::detect();
    context_.metrics = Metrics::forScale(display_->scale());
    context_.fonts = Fonts::load(display_);

    if (!window_.create(DISKSPACE_APP_NAME, display_->scaled(1100), display_->scaled(720))) {
        return false;
    }
    // The instance name has to match the desktop file's basename, or the shell
    // has nothing to match the window against and shows a default icon.
    window_.setClass("diskspace", "DiskSpace");
    window_.setMinimumSize(display_->scaled(640), display_->scaled(420));
    applyTitle();

    window_.onDraw = [this](ui::Canvas& canvas) { draw(canvas); };
    window_.onMousePress = [this](const ui::MouseEvent& event) { onMousePress(event); };
    window_.onMouseRelease = [this](const ui::MouseEvent& event) { onMouseRelease(event); };
    window_.onMouseMove = [this](const ui::MouseEvent& event) { onMouseMove(event); };
    window_.onScroll = [this](const ui::MouseEvent& event, int dx, int dy) {
        onScroll(event, dx, dy);
    };
    window_.onKey = [this](const ui::KeyEvent& event) { onKey(event); };
    window_.onMouseLeave = [this] {
        context_.pointer.x = -1000;
        context_.pointer.y = -1000;
        map_.mouseLeave();
        redraw();
    };
    window_.onResize = [this](int, int) { redraw(); };

    // One timer drives everything that has to happen without an event: the scan's
    // progress, and the update check's answer.
    window_.setTimer(100, [this] { tick(); });

    auto needsRedraw = [this] { redraw(); };
    tree_.onNeedsRedraw = needsRedraw;
    kinds_.onNeedsRedraw = needsRedraw;
    map_.onNeedsRedraw = needsRedraw;
    chooser_.onNeedsRedraw = needsRedraw;
    settings_.onNeedsRedraw = needsRedraw;

    tree_.setPalette(&palette_);
    map_.setPalette(&palette_);
    kinds_.setPalette(&palette_);

    tree_.onSelected = [this](int32_t node) { map_.setSelected(node); };
    tree_.onActivated = [this](int32_t node) { zoomTo(node); };
    tree_.onContextMenu = [this](int32_t node, int x, int y) { showContextMenu(node, x, y); };

    map_.onSelected = [this](int32_t node) { tree_.revealNode(node); };
    map_.onActivated = [this](int32_t node) { zoomTo(node); };
    map_.onContextMenu = [this](int32_t node, int x, int y) { showContextMenu(node, x, y); };
    map_.onHovered = [this](int32_t node) {
        if (node < 0 || !scan_.scanner()) {
            hoverText_.clear();
        } else {
            const diskscan::NodeArena& arena = scan_.scanner()->arena();
            if (size_t(uint32_t(node)) < arena.count()) {
                const NameString path = arena.path(size_t(uint32_t(node)));
                hoverText_ = displayName(path.c_str(), path.size()) + "   " +
                             humanBytes(arena.size(options_.metric, size_t(uint32_t(node))));
            }
        }
        redraw();
    };

    kinds_.onSelected = [this](int16_t) { redraw(); };

    chooser_.onChosen = [this](const std::string& path) { scanPath(path); };

    Preferences::shared().addObserver([this] { onPreferencesChanged(); });

    scan_.onProgress = [this] { onScanProgress(); };
    scan_.onFinished = [this] { onScanFinished(); };

    volumes_ = diskscan::mountedVolumes();
    checkForUpdates();
    return true;
}

void MainWindow::run() {
    window_.show();
    window_.run();
}

// MARK: - Painting

void MainWindow::draw(ui::Canvas& canvas) {
    const Theme& theme = context_.theme;
    canvas.fill(ui::Rect{0, 0, canvas.width(), canvas.height()}, theme.background);

    // Rebuilt every frame by whichever screen or panel draws one, so a link that
    // is no longer on screen cannot still be clickable.
    links_.clear();

    const int headerHeight = context_.metrics.headerHeight;
    const int statusHeight = context_.metrics.statusHeight;

    drawHeader(canvas, ui::Rect{0, 0, canvas.width(), headerHeight});

    // On the chooser only, which is where the other two put it. Once a scan is
    // running the notice would be competing with the thing the user came here to
    // look at, and it is there again next launch.
    int top = headerHeight;
    if (bannerVisible_ && !showingResults_) {
        const int bannerHeight = context_.metrics.rowHeight + 12;
        drawBanner(canvas, ui::Rect{0, top, canvas.width(), bannerHeight});
        top += bannerHeight;
    }

    const ui::Rect content{0, top, canvas.width(),
                           canvas.height() - top - (showingResults_ ? statusHeight : 0)};
    if (showingResults_) {
        layoutResults(content);
        tree_.draw(canvas, context_);
        map_.draw(canvas, context_);
        if (Preferences::shared().showFileKinds()) kinds_.draw(canvas, context_);

        // The splitters, as a faint line each. They are wide enough to grab and
        // narrow enough not to be furniture.
        canvas.fill(ui::Rect{outerSplitBounds_.x + outerSplitBounds_.width / 2 - 1,
                             outerSplitBounds_.y,
                             outerSplitBounds_.width > outerSplitBounds_.height ? 0 : 1,
                             outerSplitBounds_.height},
                    theme.border);
        if (outerSplitBounds_.width > outerSplitBounds_.height) {
            canvas.fill(ui::Rect{outerSplitBounds_.x,
                                 outerSplitBounds_.y + outerSplitBounds_.height / 2,
                                 outerSplitBounds_.width, 1},
                        theme.border);
        }
        if (innerSplitBounds_.width > innerSplitBounds_.height) {
            canvas.fill(ui::Rect{innerSplitBounds_.x,
                                 innerSplitBounds_.y + innerSplitBounds_.height / 2,
                                 innerSplitBounds_.width, 1},
                        theme.border);
        } else {
            canvas.fill(ui::Rect{innerSplitBounds_.x + innerSplitBounds_.width / 2,
                                 innerSplitBounds_.y, 1, innerSplitBounds_.height},
                        theme.border);
        }

        drawStatus(canvas, ui::Rect{0, canvas.height() - statusHeight, canvas.width(),
                                    statusHeight});
    } else {
        drawWelcome(canvas, content);
    }

    menu_.layout(canvas, context_.fonts.regular);
    menu_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
    chooser_.draw(canvas, context_);
    settings_.draw(canvas, context_);
    drawAbout(canvas);
    drawConfirm(canvas);
}

void MainWindow::drawHeader(ui::Canvas& canvas, const ui::Rect& bounds) {
    const Theme& theme = context_.theme;
    canvas.fill(bounds, theme.surface);
    canvas.fill(ui::Rect{bounds.x, bounds.bottom() - 1, bounds.width, 1}, theme.border);

    const int buttonHeight = context_.metrics.rowHeight + 6;
    const int y = bounds.y + (bounds.height - buttonHeight) / 2;
    int x = context_.metrics.padding;

    // Only while results are showing. On the chooser it would lead back to the
    // screen it is already on, which is why the other two hide it there too.
    if (showingResults_) {
        openButton_.label = "Open\xe2\x80\xa6";
        openButton_.bounds =
            ui::Rect{x, y, openButton_.preferredWidth(canvas, context_.fonts.regular),
                     buttonHeight};
        openButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
        x = openButton_.bounds.right() + 6;
    } else {
        openButton_.bounds = ui::Rect{};
    }

    if (showingResults_) {
        if (scan_.running()) {
            stopButton_.label = "Stop";
            stopButton_.bounds =
                ui::Rect{x, y, stopButton_.preferredWidth(canvas, context_.fonts.regular),
                         buttonHeight};
            stopButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
            x = stopButton_.bounds.right() + 6;
            rescanButton_.bounds = ui::Rect{};
        } else {
            rescanButton_.label = "Rescan";
            rescanButton_.bounds =
                ui::Rect{x, y, rescanButton_.preferredWidth(canvas, context_.fonts.regular),
                         buttonHeight};
            rescanButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
            x = rescanButton_.bounds.right() + 6;
            stopButton_.bounds = ui::Rect{};
        }

        if (mapRoot_ != 0) {
            zoomOutButton_.label = "\xe2\x86\x91";
            zoomOutButton_.bounds = ui::Rect{x, y, buttonHeight + 4, buttonHeight};
            zoomOutButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
            x = zoomOutButton_.bounds.right() + 6;
        } else {
            zoomOutButton_.bounds = ui::Rect{};
        }
    } else {
        rescanButton_.bounds = ui::Rect{};
        stopButton_.bounds = ui::Rect{};
        zoomOutButton_.bounds = ui::Rect{};
    }

    // The controls on the right, laid out before the breadcrumbs so the crumbs
    // know how much room is left.
    menuButton_.label = "\xe2\x98\xb0";
    menuButton_.flat = true;
    menuButton_.bounds =
        ui::Rect{bounds.right() - context_.metrics.padding - buttonHeight - 4, y, buttonHeight + 4,
                 buttonHeight};
    menuButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
    int right = menuButton_.bounds.x - 6;

    if (showingResults_) {
        metricButton_.label = std::string(kMetricNames[int(options_.metric)]) + "  \xe2\x96\xbe";
        const int width = metricButton_.preferredWidth(canvas, context_.fonts.regular);
        metricButton_.bounds = ui::Rect{right - width, y, width, buttonHeight};
        metricButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
        right = metricButton_.bounds.x - 8;
    } else {
        metricButton_.bounds = ui::Rect{};
    }

    // The breadcrumbs, in whatever is left between the two groups.
    crumbs_.clear();
    if (!showingResults_ || !scan_.scanner()) return;
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    if (arena.count() == 0) return;

    std::vector<int32_t> chain;
    size_t current = size_t(uint32_t(mapRoot_));
    for (;;) {
        chain.push_back(int32_t(current));
        if (current == 0) break;
        current = size_t(uint32_t(arena.parent[current]));
    }
    std::reverse(chain.begin(), chain.end());

    // From the right, so that a deep path loses its start rather than its end -
    // the folder being looked at matters more than the volume it is on.
    std::vector<std::pair<std::string, int32_t>> labels;
    for (int32_t node : chain) {
        const size_t index = size_t(uint32_t(node));
        labels.emplace_back(node == 0 ? displayName(shownRoot_.c_str(), shownRoot_.size())
                                      : displayName(arena.nameData(index), arena.nameSize(index)),
                            node);
    }

    int used = 0;
    size_t firstShown = 0;
    for (size_t i = labels.size(); i-- > 0;) {
        const int width = canvas.textWidth(labels[i].first, context_.fonts.regular) + 16;
        if (used + width > right - x && i + 1 < labels.size()) {
            firstShown = i + 1;
            break;
        }
        used += width + 12;
    }

    int crumbX = x + 4;
    for (size_t i = firstShown; i < labels.size(); ++i) {
        if (i > firstShown || firstShown > 0) {
            canvas.textIn(ui::Rect{crumbX, bounds.y, 14, bounds.height},
                          i == firstShown ? "\xe2\x80\xa6" : "\xe2\x80\xba", context_.fonts.regular,
                          theme.dimText, ui::Align::Center);
            crumbX += 14;
        }
        const int width = canvas.textWidth(labels[i].first, context_.fonts.regular) + 12;
        const ui::Rect crumb{crumbX, y, width, buttonHeight};
        if (crumb.right() > right) break;
        const bool last = i + 1 == labels.size();
        if (!last && crumb.contains(context_.pointer.x, context_.pointer.y)) {
            canvas.fillRounded(crumb, 5, theme.surfaceHover);
        }
        canvas.textIn(crumb, labels[i].first, last ? context_.fonts.bold : context_.fonts.regular,
                      last ? theme.text : theme.dimText, ui::Align::Center);
        if (!last) crumbs_.emplace_back(crumb, labels[i].second);
        crumbX = crumb.right();
    }
}

void MainWindow::drawBanner(ui::Canvas& canvas, const ui::Rect& bounds) {
    const Theme& theme = context_.theme;
    canvas.fill(bounds, theme.dark ? ui::rgb(0x2b, 0x3b, 0x55) : ui::rgb(0xdd, 0xe9, 0xfb));
    canvas.fill(ui::Rect{bounds.x, bounds.bottom() - 1, bounds.width, 1}, theme.border);

    // No way to dismiss it, which is deliberate: neither of the other two builds
    // offers one, and the notice goes away by itself the moment a scan starts.
    const int buttonHeight = bounds.height - 6;
    bannerDismiss_.bounds = ui::Rect{};
    int right = bounds.right() - context_.metrics.padding - 4;
    if (!bannerUrl_.empty()) {
        bannerButton_.label = "Download";
        const int width = bannerButton_.preferredWidth(canvas, context_.fonts.regular);
        bannerButton_.bounds = ui::Rect{right - width, bounds.y + 3, width, buttonHeight};
        bannerButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
        right = bannerButton_.bounds.x - 8;
    } else {
        bannerButton_.bounds = ui::Rect{};
    }

    canvas.textIn(ui::Rect{bounds.x + context_.metrics.padding + 4, bounds.y,
                           right - bounds.x - context_.metrics.padding, bounds.height},
                  bannerText_, context_.fonts.regular, theme.text);
}

void MainWindow::layoutResults(const ui::Rect& bounds) {
    const bool horizontal = Preferences::shared().splitHorizontally();
    const bool showKinds = Preferences::shared().showFileKinds();

    // The inner split runs across the outer one. With the map beside the list,
    // the kind table sits under the list; with the map below, it sits beside it.
    // Nesting two splits the same way would stack three panes in a column and
    // leave the kind table one row tall.
    if (outerSplit_ <= 0) outerSplit_ = display_->scaled(horizontal ? 340 : 470);
    if (innerSplit_ <= 0) innerSplit_ = display_->scaled(horizontal ? 640 : 430);

    ui::Rect listSide;
    ui::Rect mapSide;
    if (horizontal) {
        const int split = std::clamp(outerSplit_, 120, std::max(120, bounds.height - 160));
        listSide = ui::Rect{bounds.x, bounds.y, bounds.width, split};
        mapSide = ui::Rect{bounds.x, bounds.y + split + kSplitterWidth, bounds.width,
                           bounds.height - split - kSplitterWidth};
        outerSplitBounds_ = ui::Rect{bounds.x, bounds.y + split, bounds.width, kSplitterWidth};
    } else {
        const int split = std::clamp(outerSplit_, 200, std::max(200, bounds.width - 240));
        listSide = ui::Rect{bounds.x, bounds.y, split, bounds.height};
        mapSide = ui::Rect{bounds.x + split + kSplitterWidth, bounds.y,
                           bounds.width - split - kSplitterWidth, bounds.height};
        outerSplitBounds_ = ui::Rect{bounds.x + split, bounds.y, kSplitterWidth, bounds.height};
    }

    if (!showKinds) {
        tree_.setBounds(listSide);
        kinds_.setBounds(ui::Rect{});
        innerSplitBounds_ = ui::Rect{};
    } else if (horizontal) {
        const int split = std::clamp(innerSplit_, 200, std::max(200, listSide.width - 200));
        tree_.setBounds(ui::Rect{listSide.x, listSide.y, split, listSide.height});
        kinds_.setBounds(ui::Rect{listSide.x + split + kSplitterWidth, listSide.y,
                                  listSide.width - split - kSplitterWidth, listSide.height});
        innerSplitBounds_ =
            ui::Rect{listSide.x + split, listSide.y, kSplitterWidth, listSide.height};
    } else {
        const int split = std::clamp(innerSplit_, 120, std::max(120, listSide.height - 120));
        tree_.setBounds(ui::Rect{listSide.x, listSide.y, listSide.width, split});
        kinds_.setBounds(ui::Rect{listSide.x, listSide.y + split + kSplitterWidth, listSide.width,
                                  listSide.height - split - kSplitterWidth});
        innerSplitBounds_ = ui::Rect{listSide.x, listSide.y + split, listSide.width,
                                     kSplitterWidth};
    }
    map_.setBounds(mapSide);
}

void MainWindow::drawWelcome(ui::Canvas& canvas, const ui::Rect& bounds) {
    const Theme& theme = context_.theme;
    const int padding = context_.metrics.padding;
    const int rowHeight = context_.metrics.rowHeight;

    const int cardWidth = std::min(display_->scaled(330), (bounds.width - padding * 5) / 2);
    const int cardHeight = rowHeight * 3 + 18;
    const int columns = std::max(1, (bounds.width - padding * 2) / (cardWidth + padding));
    const int gridWidth = columns * cardWidth + (columns - 1) * padding;
    int x = bounds.x + (bounds.width - gridWidth) / 2;
    int y = bounds.y + rowHeight * 2;

    // The same header the macOS build has: the icon, the application's name and
    // what to do next on one line, and one line under it saying what to expect.
    //
    // Aligned to the left of the card grid rather than centred on the window, so
    // the whole screen reads as one column - which is what makes the macOS one
    // look deliberate rather than floated.
    const std::string heading = std::string(DISKSPACE_APP_NAME) + " - choose a folder to measure";
    const std::string subtitle = "Scanning is incremental — results appear as they are found.";
    const int iconSize = display_->scaled(44);
    const int gap = display_->scaled(12);
    const ui::Font& headingFont = context_.fonts.heading;

    icon_.draw(canvas, x, y, iconSize, theme.background);
    canvas.textIn(ui::Rect{x + iconSize + gap, y, gridWidth - iconSize - gap, iconSize}, heading,
                  headingFont, theme.text);
    y += iconSize + display_->scaled(8);

    canvas.textIn(ui::Rect{x, y, gridWidth, rowHeight}, subtitle, context_.fonts.regular,
                  theme.dimText);
    y += rowHeight + display_->scaled(20);

    volumeCards_.clear();
    int column = 0;
    for (const diskscan::VolumeInfo& volume : volumes_) {
        const ui::Rect card{x + column * (cardWidth + padding), y, cardWidth, cardHeight};
        if (card.bottom() > bounds.bottom() - rowHeight * 3) break;

        const bool hovered = card.contains(context_.pointer.x, context_.pointer.y);
        canvas.fillRounded(card, 8, hovered ? theme.surfaceHover : theme.surface);
        canvas.frame(card, theme.border);

        canvas.textIn(ui::Rect{card.x + 12, card.y + 8, card.width - 24, rowHeight},
                      displayName(volume.mountPoint.c_str(), volume.mountPoint.size()),
                      context_.fonts.bold, theme.text);
        const std::string detail = humanBytes(volume.usedBytes) + " used of " +
                                   humanBytes(volume.totalBytes) + "  \xc2\xb7  " +
                                   volume.fileSystem;
        canvas.textIn(ui::Rect{card.x + 12, card.y + 8 + rowHeight, card.width - 24, rowHeight},
                      detail, context_.fonts.small, theme.dimText);

        // How full it is, as a bar.
        const ui::Rect track{card.x + 12, card.bottom() - 16, card.width - 24, 6};
        canvas.fillRounded(track, 3, theme.dark ? ui::rgb(0x1c, 0x1c, 0x20)
                                                : ui::rgb(0xe2, 0xe1, 0xe0));
        if (volume.totalBytes > 0) {
            const double fraction = double(volume.usedBytes) / double(volume.totalBytes);
            const int filled = std::max(3, int(track.width * std::clamp(fraction, 0.0, 1.0)));
            canvas.fillRounded(ui::Rect{track.x, track.y, filled, track.height}, 3,
                               fraction > 0.9 ? ui::rgb(0xe0, 0x62, 0x4a) : theme.accent);
        }

        volumeCards_.emplace_back(card, diskscan::volumeScanPath(volume));
        if (++column == columns) {
            column = 0;
            y += cardHeight + padding;
        }
    }
    if (column != 0) y += cardHeight + padding;

    chooseFolderButton_.label = "Choose a Folder\xe2\x80\xa6";
    const int width = chooseFolderButton_.preferredWidth(canvas, context_.fonts.regular) + 20;
    // Left, with everything else: the header, the cards and this are one column
    // now, and a centred button in the middle of it would break the line they
    // share.
    chooseFolderButton_.bounds = ui::Rect{x, y + 8, width, rowHeight + 8};
    chooseFolderButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);

    drawFooter(canvas, bounds);
}

std::string MainWindow::statusText() const {
    if (!scan_.scanner()) return std::string();
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    const size_t count = arena.count();
    if (count == 0) return "Scanning\xe2\x80\xa6";

    const int64_t total =
        options_.useLogicalSize ? arena.logical[0] : arena.size(options_.metric, 0);
    std::string text = humanBytes(total) + " in " + grouped(int64_t(count)) + " items";
    if (scan_.running()) {
        text += "  \xc2\xb7  scanning\xe2\x80\xa6";
    } else {
        const diskscan::ScanResult& result = scan_.result();
        char elapsed[64];
        std::snprintf(elapsed, sizeof(elapsed), "  \xc2\xb7  %.2fs", result.elapsed);
        text += elapsed;
        if (result.hardlinkDuplicates > 0) {
            text += "  \xc2\xb7  " + grouped(result.hardlinkDuplicates) +
                    " hard links counted once (" + humanBytes(result.hardlinkDuplicateBytes) + ")";
        }
        if (result.errors > 0) text += "  \xc2\xb7  " + grouped(result.errors) + " unreadable";
        if (result.stopped) text += "  \xc2\xb7  stopped";
    }
    return text;
}

void MainWindow::drawStatus(ui::Canvas& canvas, const ui::Rect& bounds) {
    const Theme& theme = context_.theme;
    canvas.fill(bounds, theme.surface);
    canvas.fill(ui::Rect{bounds.x, bounds.y, bounds.width, 1}, theme.border);

    const int padding = context_.metrics.padding + 4;
    const std::string status = statusText();
    const int statusWidth = canvas.textWidth(status, context_.fonts.small) + 20;
    canvas.textIn(ui::Rect{bounds.x + padding, bounds.y, statusWidth, bounds.height}, status,
                  context_.fonts.small, theme.dimText);

    if (!hoverText_.empty()) {
        const int left = bounds.x + padding + statusWidth;
        canvas.textIn(ui::Rect{left, bounds.y, bounds.right() - left - padding, bounds.height},
                      hoverText_, context_.fonts.small, theme.text, ui::Align::Right);
    }
}

void MainWindow::drawConfirm(ui::Canvas& canvas) {
    if (!confirm_.open) return;
    const Theme& theme = context_.theme;
    canvas.fill(ui::Rect{0, 0, canvas.width(), canvas.height()}, ui::rgba(0, 0, 0, 0x66));

    const int rowHeight = context_.metrics.rowHeight;
    const int padding = context_.metrics.padding;
    const int width = std::min(canvas.width() - 80, rowHeight * 22);
    const int height = rowHeight * 3 + padding * 4 + 10;
    confirm_.panel =
        ui::Rect{(canvas.width() - width) / 2, (canvas.height() - height) / 2, width, height};

    canvas.fillRounded(confirm_.panel, 10, theme.background);
    canvas.frame(confirm_.panel, theme.border);

    canvas.textIn(ui::Rect{confirm_.panel.x + padding + 4, confirm_.panel.y + padding,
                           confirm_.panel.width - 2 * padding, rowHeight + 4},
                  confirm_.title, context_.fonts.bold, theme.text);
    canvas.textIn(ui::Rect{confirm_.panel.x + padding + 4, confirm_.panel.y + padding + rowHeight + 6,
                           confirm_.panel.width - 2 * padding, rowHeight},
                  confirm_.detail, context_.fonts.small, theme.dimText);

    const int y = confirm_.panel.bottom() - padding - rowHeight - 6;
    confirm_.confirmButton.label = confirm_.confirmLabel;
    confirm_.cancelButton.label = "Cancel";
    const int confirmWidth =
        confirm_.confirmButton.preferredWidth(canvas, context_.fonts.regular) + 16;
    const int cancelWidth =
        confirm_.cancelButton.preferredWidth(canvas, context_.fonts.regular) + 16;
    confirm_.confirmButton.bounds = ui::Rect{confirm_.panel.right() - padding - confirmWidth, y,
                                             confirmWidth, rowHeight + 6};
    confirm_.cancelButton.bounds = ui::Rect{confirm_.confirmButton.bounds.x - cancelWidth - 8, y,
                                            cancelWidth, rowHeight + 6};
    confirm_.cancelButton.draw(canvas, theme, context_.pointer, context_.fonts.regular);

    if (confirm_.destructive) {
        // Coloured, because the button that cannot be undone should not look the
        // same as the one beside it.
        const bool hovered =
            confirm_.confirmButton.bounds.contains(context_.pointer.x, context_.pointer.y);
        canvas.fillRounded(confirm_.confirmButton.bounds, 5,
                           hovered ? ui::rgb(0xc0, 0x39, 0x2b) : ui::rgb(0xd0, 0x45, 0x37));
        canvas.textIn(confirm_.confirmButton.bounds, confirm_.confirmLabel, context_.fonts.regular,
                      ui::rgb(0xff, 0xff, 0xff), ui::Align::Center);
    } else {
        confirm_.confirmButton.draw(canvas, theme, context_.pointer, context_.fonts.regular);
    }
}

int MainWindow::drawLink(ui::Canvas& canvas, int x, int y, int height, const std::string& text,
                         const std::string& url, const ui::Font& font) {
    const int width = canvas.textWidth(text, font);
    const ui::Rect bounds{x, y, width, height};
    const bool hovered = bounds.contains(context_.pointer.x, context_.pointer.y);

    canvas.textIn(bounds, text, font, context_.theme.accent);
    if (hovered) {
        // Underlined on hover rather than always. A footer with two permanently
        // underlined phrases in it reads as a form, not a signature.
        const int baseline = y + (height - font.lineHeight) / 2 + font.ascent + 2;
        canvas.fill(ui::Rect{x, baseline, width, 1}, context_.theme.accent);
    }
    links_.push_back(Link{bounds, url});
    return width;
}

void MainWindow::drawFooter(ui::Canvas& canvas, const ui::Rect& bounds) {
    const ui::Font& font = context_.fonts.small;
    const int height = font.lineHeight + 6;

    // Measured first so the whole line can be centred, since it is three pieces
    // and only the middle one is fixed.
    const std::string prefix = "Built by ";
    const std::string separator = "  ·  ";
    const int total = canvas.textWidth(prefix, font) + canvas.textWidth("Chofter Apps", font) +
                      canvas.textWidth(separator, font) + canvas.textWidth("diskspace.io", font);

    int x = bounds.x + (bounds.width - total) / 2;
    const int y = bounds.bottom() - height - context_.metrics.padding;

    canvas.textIn(ui::Rect{x, y, canvas.textWidth(prefix, font), height}, prefix, font,
                  context_.theme.dimText);
    x += canvas.textWidth(prefix, font);
    x += drawLink(canvas, x, y, height, "Chofter Apps", "https://www.chofter.com", font);
    canvas.textIn(ui::Rect{x, y, canvas.textWidth(separator, font), height}, separator, font,
                  context_.theme.dimText);
    x += canvas.textWidth(separator, font);
    drawLink(canvas, x, y, height, "diskspace.io", "https://diskspace.io", font);
}

void MainWindow::drawAbout(ui::Canvas& canvas) {
    if (!aboutOpen_) return;
    const Theme& theme = context_.theme;
    canvas.fill(ui::Rect{0, 0, canvas.width(), canvas.height()}, ui::rgba(0, 0, 0, 0x66));

    const int rowHeight = context_.metrics.rowHeight;
    const int padding = context_.metrics.padding * 2;
    const int width = std::min(canvas.width() - 80, rowHeight * 22);
    const int iconSize = display_->scaled(64);

    // The same words as the macOS and Windows About boxes, so all three describe
    // the application the same way.
    const std::string description =
        "Find out where your disk space went. Scans a folder or a whole volume in parallel "
        "and shows the results as they arrive, with a treemap of every file and four "
        "measures of size — including what has only appeared recently.";

    const int textWidth = width - padding * 2;
    const int descriptionHeight =
        int(canvas.wrap(description, context_.fonts.regular, textWidth).size()) *
        (context_.fonts.regular.lineHeight + 2);

    // The update notice, when there is one, and whether it fits beside its link.
    //
    // It usually does not: "Version 1.4.2 is available. Yours is 5 months old."
    // plus "Download it" is wider than this panel, and drawn on one line the link
    // ran off the right edge. Measured here so the panel can be made tall enough
    // for the wrapped version before anything is drawn into it.
    const UpdateAvailable* update = UpdateCheck::shared().result();
    const std::string notice = update ? update->summary() : std::string();
    const int linkWidth = canvas.textWidth("Download it", context_.fonts.bold);
    const bool updateOnOneLine =
        update && canvas.textWidth(notice + " ", context_.fonts.bold) + linkWidth <= textWidth;
    const int updateLines =
        !update ? 0
                : (updateOnOneLine
                       ? 1
                       : int(canvas.wrap(notice, context_.fonts.bold, textWidth).size()) + 1);
    const int updateHeight = update ? updateLines * rowHeight + 8 : 0;

    const int height = padding + iconSize + 10 + rowHeight + rowHeight + 10 + descriptionHeight +
                       10 + updateHeight + rowHeight + 12 + rowHeight + 6 + padding;
    aboutPanel_ = ui::Rect{(canvas.width() - width) / 2, (canvas.height() - height) / 2, width,
                           height};

    canvas.fillRounded(aboutPanel_, 10, theme.background);
    canvas.frame(aboutPanel_, theme.border);

    int y = aboutPanel_.y + padding;
    icon_.draw(canvas, aboutPanel_.x + (width - iconSize) / 2, y, iconSize, theme.background);
    y += iconSize + 10;

    canvas.textIn(ui::Rect{aboutPanel_.x, y, width, rowHeight}, DISKSPACE_APP_NAME,
                  context_.fonts.title, theme.text, ui::Align::Center);
    y += rowHeight;
    canvas.textIn(ui::Rect{aboutPanel_.x, y, width, rowHeight},
                  "Version " + UpdateCheck::currentVersion(), context_.fonts.small, theme.dimText,
                  ui::Align::Center);
    y += rowHeight + 10;

    y += canvas.textBlock(ui::Rect{aboutPanel_.x + padding, y, textWidth, descriptionHeight},
                          description, context_.fonts.regular, theme.text, 2, ui::Align::Center);
    y += 10;

    if (update && updateOnOneLine) {
        const std::string spaced = notice + " ";
        const int noticeWidth = canvas.textWidth(spaced, context_.fonts.bold);
        const int left = aboutPanel_.x + (width - noticeWidth - linkWidth) / 2;
        canvas.textIn(ui::Rect{left, y, noticeWidth, rowHeight}, spaced, context_.fonts.bold,
                      theme.text);
        drawLink(canvas, left + noticeWidth, y, rowHeight, "Download it", update->downloads,
                 context_.fonts.bold);
        y += rowHeight + 8;
    } else if (update) {
        // Wrapped, with the link on its own line under it and centred like
        // everything else in this panel.
        for (const std::string& line : canvas.wrap(notice, context_.fonts.bold, textWidth)) {
            canvas.textIn(ui::Rect{aboutPanel_.x + padding, y, textWidth, rowHeight}, line,
                          context_.fonts.bold, theme.text, ui::Align::Center);
            y += rowHeight;
        }
        drawLink(canvas, aboutPanel_.x + (width - linkWidth) / 2, y, rowHeight, "Download it",
                 update->downloads, context_.fonts.bold);
        y += rowHeight + 8;
    }

    // The same signature as the welcome screen's footer, in the same words.
    const ui::Font& small = context_.fonts.small;
    const std::string prefix = "Built by ";
    const std::string separator = "  ·  ";
    const int total = canvas.textWidth(prefix, small) + canvas.textWidth("Chofter Apps", small) +
                      canvas.textWidth(separator, small) + canvas.textWidth("diskspace.io", small);
    int x = aboutPanel_.x + (width - total) / 2;
    canvas.textIn(ui::Rect{x, y, canvas.textWidth(prefix, small), rowHeight}, prefix, small,
                  theme.dimText);
    x += canvas.textWidth(prefix, small);
    x += drawLink(canvas, x, y, rowHeight, "Chofter Apps", "https://www.chofter.com", small);
    canvas.textIn(ui::Rect{x, y, canvas.textWidth(separator, small), rowHeight}, separator, small,
                  theme.dimText);
    x += canvas.textWidth(separator, small);
    drawLink(canvas, x, y, rowHeight, "diskspace.io", "https://diskspace.io", small);
    y += rowHeight + 12;

    aboutCloseButton_.label = "Close";
    const int closeWidth = aboutCloseButton_.preferredWidth(canvas, context_.fonts.regular) + 16;
    aboutCloseButton_.bounds =
        ui::Rect{aboutPanel_.x + (width - closeWidth) / 2, y, closeWidth, rowHeight + 6};
    aboutCloseButton_.draw(canvas, theme, context_.pointer, context_.fonts.regular);
}

void MainWindow::applyTitle() {
    // "Disk Space · 1.0.0", with the path in front of it once there is one.
    //
    // The GNOME convention is to title a window after its document and let the
    // shell supply the application's name from the desktop file. That is right
    // where a shell does so, and it leaves the window unidentifiable where one
    // does not: in a plain window list the title is the only thing shown, and a
    // window called "/" tells nobody what it is.
    std::string title = std::string(DISKSPACE_APP_NAME) + " · " +
                        UpdateCheck::currentVersion();
    if (showingResults_ && !shownRoot_.empty()) {
        title = displayName(shownRoot_.c_str(), shownRoot_.size()) + " — " + title;
    }
    window_.setTitle(title);
}

// MARK: - Events

void MainWindow::onMousePress(const ui::MouseEvent& event) {
    context_.pointer.x = event.x;
    context_.pointer.y = event.y;
    context_.pointer.down = true;

    // Front to back: whatever is on top gets first refusal.
    //
    // Links come before any of it. They are drawn by several different screens
    // and panels, and wherever one is drawn it is the topmost thing at that
    // point, so a single check here saves every one of them from repeating it.
    if (event.button == 1) {
        for (const Link& link : links_) {
            if (link.bounds.contains(event.x, event.y)) {
                shell::openPath(link.url);
                return;
            }
        }
    }

    if (aboutOpen_) {
        if (aboutCloseButton_.hit(event.x, event.y) || !aboutPanel_.contains(event.x, event.y)) {
            aboutOpen_ = false;
            redraw();
        }
        return;
    }
    if (confirm_.open) {
        if (confirm_.cancelButton.hit(event.x, event.y) || !confirm_.panel.contains(event.x, event.y)) {
            confirm_.open = false;
        } else if (confirm_.confirmButton.hit(event.x, event.y)) {
            confirm_.open = false;
            if (confirm_.onConfirm) confirm_.onConfirm();
        }
        redraw();
        return;
    }
    if (settings_.isOpen()) {
        settings_.mousePress(event.x, event.y, event.button, context_);
        return;
    }
    if (chooser_.isOpen()) {
        chooser_.mousePress(event.x, event.y, event.button, event.clicks, context_);
        redraw();
        return;
    }
    if (menu_.isOpen()) {
        menu_.click(event.x, event.y);
        redraw();
        return;
    }

    if (openButton_.hit(event.x, event.y)) {
        showWelcome();
        return;
    }
    if (menuButton_.hit(event.x, event.y)) {
        showApplicationMenu(menuButton_.bounds.x, menuButton_.bounds.bottom() + 2);
        return;
    }
    if (metricButton_.hit(event.x, event.y)) {
        showMetricMenu(metricButton_.bounds.x, metricButton_.bounds.bottom() + 2);
        return;
    }
    if (rescanButton_.hit(event.x, event.y)) {
        rescan();
        return;
    }
    if (stopButton_.hit(event.x, event.y)) {
        scan_.stop();
        return;
    }
    if (zoomOutButton_.hit(event.x, event.y)) {
        zoomOut();
        return;
    }
    for (const auto& [rect, node] : crumbs_) {
        if (rect.contains(event.x, event.y)) {
            if (node == 0) {
                mapRoot_ = 0;
                map_.setRoot(0);
                redraw();
            } else {
                zoomTo(node);
            }
            return;
        }
    }

    if (bannerVisible_ && !showingResults_ && bannerButton_.hit(event.x, event.y)) {
        shell::openPath(bannerUrl_);
        return;
    }

    if (!showingResults_) {
        if (chooseFolderButton_.hit(event.x, event.y)) {
            chooseFolder();
            return;
        }
        for (const auto& [rect, path] : volumeCards_) {
            if (rect.contains(event.x, event.y)) {
                scanPath(path);
                return;
            }
        }
        return;
    }

    if (outerSplitBounds_.contains(event.x, event.y)) {
        draggingSplit_ = 1;
        return;
    }
    if (innerSplitBounds_.contains(event.x, event.y)) {
        draggingSplit_ = 2;
        return;
    }

    if (tree_.mousePress(event.x, event.y, event.button, event.clicks, context_)) return;
    if (Preferences::shared().showFileKinds() &&
        kinds_.mousePress(event.x, event.y, event.button, context_)) {
        return;
    }
    map_.mousePress(event.x, event.y, event.button, event.clicks);
}

void MainWindow::onMouseRelease(const ui::MouseEvent& event) {
    context_.pointer.down = false;
    draggingSplit_ = 0;
    tree_.mouseRelease();
    kinds_.mouseRelease();
    chooser_.mouseRelease();
    redraw();
}

void MainWindow::onMouseMove(const ui::MouseEvent& event) {
    context_.pointer.x = event.x;
    context_.pointer.y = event.y;

    if (draggingSplit_ != 0) {
        const bool horizontal = Preferences::shared().splitHorizontally();
        // The banner is never up while results are showing, so it adds nothing
        // to the offset a splitter drag has to account for.
        const int headerHeight = context_.metrics.headerHeight;
        if (draggingSplit_ == 1) {
            outerSplit_ = horizontal ? event.y - headerHeight : event.x;
        } else {
            innerSplit_ = horizontal ? event.x : event.y - headerHeight - (horizontal ? 0 : 0);
            if (!horizontal) innerSplit_ = event.y - headerHeight;
        }
        redraw();
        return;
    }

    if (chooser_.isOpen()) {
        chooser_.mouseDrag(event.x, event.y);
        redraw();
        return;
    }
    if (tree_.mouseDrag(event.x, event.y)) return;
    if (kinds_.mouseDrag(event.x, event.y)) return;
    if (showingResults_) map_.mouseMove(event.x, event.y);
    redraw();
}

void MainWindow::onScroll(const ui::MouseEvent& event, int dx, int dy) {
    if (chooser_.isOpen()) {
        chooser_.scroll(event.x, event.y, dy, context_);
        return;
    }
    if (!showingResults_) return;
    if (tree_.scroll(event.x, event.y, dy, context_)) return;
    if (Preferences::shared().showFileKinds()) kinds_.scroll(event.x, event.y, dy, context_);
}

void MainWindow::onKey(const ui::KeyEvent& event) {
    if (aboutOpen_) {
        if (event.key == XK_Escape || event.key == XK_Return) {
            aboutOpen_ = false;
            redraw();
        }
        return;
    }
    if (confirm_.open) {
        if (event.key == XK_Escape) confirm_.open = false;
        if (event.key == XK_Return && confirm_.onConfirm) {
            confirm_.open = false;
            confirm_.onConfirm();
        }
        redraw();
        return;
    }
    if (settings_.isOpen()) {
        settings_.key(event.key);
        return;
    }
    if (chooser_.isOpen()) {
        chooser_.key(event, context_);
        redraw();
        return;
    }
    if (menu_.isOpen()) {
        if (event.key == XK_Escape) {
            menu_.close();
            redraw();
        }
        return;
    }

    if (event.control) {
        switch (event.key) {
            case XK_o: case XK_O: showWelcome(); return;
            case XK_r: case XK_R: rescan(); return;
            case XK_c: case XK_C: copyPath(); return;
            case XK_q: case XK_Q: window_.quit(); return;
            case XK_comma: settings_.open(); redraw(); return;
            default: break;
        }
    }
    switch (event.key) {
        case XK_Escape:
            if (showingResults_ && mapRoot_ != 0) zoomOut();
            return;
        case XK_BackSpace:
            zoomOut();
            return;
        case XK_Delete:
            moveToTrash();
            return;
        case XK_F5:
            rescan();
            return;
        case XK_F1:
            // The conventional key for it, and the only way in besides the menu.
            checkForUpdates();
            aboutOpen_ = true;
            redraw();
            return;
        default:
            break;
    }
    if (showingResults_) tree_.key(event.key, context_);
}

void MainWindow::tick() {
    scan_.poll();
    UpdateCheck::shared().poll();

    if (!capturePath_.empty()) {
        // The timer fires ten times a second, so this counts ticks rather than
        // consulting a clock. Waiting for the scan to finish as well, since a
        // half-drawn treemap is not what anyone wants a picture of.
        ++captureTicks_;
        const bool waited = captureTicks_ >= int(captureAfter_ * 10);
        if (waited && !scan_.running()) {
            redraw();
            // The path goes into the lambda by value: it is cleared below so the
            // capture cannot be armed twice, and the posted work runs after that.
            const std::string path = capturePath_;
            window_.post([this, path] {
                if (!window_.capture(path)) {
                    std::fprintf(stderr, "could not write %s\n", path.c_str());
                }
                window_.quit();
            });
            capturePath_.clear();
        }
    }
}

void MainWindow::captureAndExit(const std::string& path, double afterSeconds) {
    capturePath_ = path;
    captureAfter_ = afterSeconds;
    captureTicks_ = 0;
}

// MARK: - Scanning

void MainWindow::showWelcome() {
    if (!showingResults_) return;

    // Nothing leads back to the results from here, so a scan still running would
    // be publishing into views no one can reach. Stopped rather than left to
    // finish, which is what the macOS and Windows builds do for the same reason.
    scan_.stop();

    // The panes let go of the arena before anything else. They hold a bare
    // pointer into the scanner, and the next scan replaces the whole object.
    tree_.setArena(nullptr);
    map_.setArena(nullptr);
    kinds_.setScanner(nullptr);
    kinds_.refresh();

    showingResults_ = false;
    shownRoot_.clear();
    mapRoot_ = 0;
    menuTarget_ = -1;
    hoverText_.clear();
    haveVolume_ = false;

    // Re-read, because the point of coming back here is usually that something
    // was deleted or a drive was plugged in.
    volumes_ = diskscan::mountedVolumes();
    applyTitle();
    redraw();
}

void MainWindow::scanPath(const NameString& path) {
    shownRoot_ = path;
    mapRoot_ = 0;
    menuTarget_ = -1;
    hoverText_.clear();
    haveVolume_ = diskscan::volumeInfoForPath(path, &volume_);
    showingResults_ = true;

    applyTitle();

    scan_.start(path);

    // Pointed at the new scan's arena, which is a different object from the last
    // one - the panes would otherwise be reading freed memory.
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    tree_.setArena(&arena);
    tree_.setRoot(0);
    map_.setArena(&arena);
    map_.setRoot(0);
    map_.setSelected(-1);
    kinds_.setScanner(scan_.scanner());
    applyOptions();
    redraw();
}

void MainWindow::onScanProgress() {
    refreshViews();
    redraw();
}

void MainWindow::onScanFinished() {
    // The volume's free space has moved if anything was deleted during the scan,
    // and the blocks drawn beside the map are sized from it.
    if (!shownRoot_.empty()) haveVolume_ = diskscan::volumeInfoForPath(shownRoot_, &volume_);
    applyOptions();
    refreshViews();
    redraw();
}

void MainWindow::refreshViews() {
    if (!scan_.scanner()) return;

    kinds_.refresh();
    // The palette hands its most distinct hues to whatever fills the most space,
    // so it has to be told the ranking before either pane draws with it. The kind
    // table's rows are already in that order under its default sort; asked for
    // separately here so a user sorting the table by name cannot recolour the map.
    std::vector<diskscan::TreeScanner::KindTotal> ranked =
        scan_.scanner()->kindTotals(options_.metric, options_.useLogicalSize);
    std::sort(ranked.begin(), ranked.end(),
              [](const diskscan::TreeScanner::KindTotal& a,
                 const diskscan::TreeScanner::KindTotal& b) { return a.bytes > b.bytes; });
    std::vector<int16_t> ranking;
    ranking.reserve(ranked.size());
    for (const auto& kind : ranked) ranking.push_back(kind.kind);
    palette_.setRanking(ranking);

    tree_.refresh();
    map_.invalidate();
}

void MainWindow::applyOptions() {
    const Preferences& preferences = Preferences::shared();
    options_.useLogicalSize = !preferences.showPhysicalFileSize();
    options_.extraBlocks = extraBlocks();
    palette_.consistentColours = preferences.consistentKindColours();

    tree_.setUseLogicalSize(options_.useLogicalSize);
    kinds_.setUseLogicalSize(options_.useLogicalSize);
    map_.setOptions(options_);
}

void MainWindow::onPreferencesChanged() {
    context_.theme = Theme::detect();
    applyOptions();
    refreshViews();
    redraw();
}

std::vector<ExtraBlock> MainWindow::extraBlocks() const {
    std::vector<ExtraBlock> blocks;
    if (!haveVolume_ || volume_.totalBytes <= 0 || !scan_.scanner()) return blocks;
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    if (arena.count() == 0) return blocks;

    const Preferences& preferences = Preferences::shared();
    if (preferences.showFreeSpace()) {
        ExtraBlock block;
        block.id = kFreeSpaceId;
        block.label = "Free space";
        block.bytes = volume_.freeBytes;
        block.colour = 0x2B3648;
        blocks.push_back(block);
    }
    if (preferences.showOtherSpace()) {
        // What is in use on the volume but outside what was scanned. Nothing, and
        // so not drawn, when a whole volume is on screen.
        const int64_t elsewhere = volume_.usedBytes - arena.allocated[0];
        if (elsewhere > 0) {
            ExtraBlock block;
            block.id = kOtherSpaceId;
            block.label = "Other space in use";
            block.bytes = elsewhere;
            block.colour = 0x4A4458;
            blocks.push_back(block);
        }
    }
    return blocks;
}

void MainWindow::setMetric(SizeMetric metric) {
    options_.metric = metric;
    tree_.setMetric(metric);
    kinds_.setMetric(metric);
    map_.setOptions(options_);
    refreshViews();
    redraw();
}

// MARK: - Zooming

void MainWindow::zoomTo(int32_t node) {
    if (!scan_.scanner() || node <= 0) return;
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    if (size_t(uint32_t(node)) >= arena.count()) return;
    if (!arena.isDirectory(size_t(uint32_t(node)))) return;

    mapRoot_ = node;
    map_.setRoot(node);
    tree_.revealNode(node);
    redraw();
}

void MainWindow::zoomOut() {
    if (!scan_.scanner() || mapRoot_ == 0) return;
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    const int32_t parent = arena.parent[size_t(uint32_t(mapRoot_))];
    // zoomTo declines node 0, which is what it should do for a double click on
    // the background - but backing all the way out is exactly how the root is
    // reached, so it is said here instead.
    if (parent == 0) {
        mapRoot_ = 0;
        map_.setRoot(0);
        tree_.revealNode(0);
        redraw();
        return;
    }
    zoomTo(parent);
}

// MARK: - Actions

int32_t MainWindow::actionTarget() const {
    if (menuTarget_ >= 0) return menuTarget_;
    const int32_t selected = tree_.selected();
    return selected >= 0 ? selected : mapRoot_;
}

NameString MainWindow::pathOf(int32_t node) const {
    if (!scan_.scanner() || node < 0) return NameString();
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    if (size_t(uint32_t(node)) >= arena.count()) return NameString();
    return arena.path(size_t(uint32_t(node)));
}

void MainWindow::copyPath() {
    const NameString path = pathOf(actionTarget());
    if (path.empty()) return;
    // Through the display form: a clipboard is text, and a name that is not valid
    // UTF-8 cannot be put on one as it stands.
    window_.setClipboardText(displayName(path.c_str(), path.size()));
}

void MainWindow::openSelected() {
    const NameString path = pathOf(actionTarget());
    if (!path.empty()) shell::openPath(path);
}

void MainWindow::revealSelected() {
    const NameString path = pathOf(actionTarget());
    if (!path.empty()) shell::revealPath(path);
}

void MainWindow::ask(const std::string& title, const std::string& detail,
                     const std::string& confirmLabel, bool destructive,
                     std::function<void()> action) {
    confirm_.open = true;
    confirm_.title = title;
    confirm_.detail = detail;
    confirm_.confirmLabel = confirmLabel;
    confirm_.destructive = destructive;
    confirm_.onConfirm = std::move(action);
    redraw();
}

void MainWindow::moveToTrash() {
    const int32_t node = actionTarget();
    const NameString path = pathOf(node);
    if (path.empty() || node <= 0) return;

    ask("Move to trash?", displayName(path.c_str(), path.size()), "Move to Trash", true,
        [this, node] {
            const NameString target = pathOf(node);
            if (target.empty()) return;
            std::string error;
            if (shell::moveToTrash(target, &error)) {
                // Taken out of every figure rather than rescanned: the entry stays
                // in the tree at zero, which is what the other builds do.
                scan_.scanner()->remove(node);
                refreshViews();
                redraw();
            } else {
                ask("Could not move it to the trash.", error, "OK", false, nullptr);
            }
        });
}

void MainWindow::showContextMenu(int32_t node, int x, int y) {
    menuTarget_ = node;
    const bool haveTarget = node > 0;
    menu_.open(
        {
            {"Copy Path", haveTarget, false, false, [this] { copyPath(); }},
            {"Open", haveTarget, false, false, [this] { openSelected(); }},
            {"Show in Files", haveTarget, false, false, [this] { revealSelected(); }},
            {"", true, false, false, nullptr},
            {"Rescan This Folder", haveTarget && !scan_.running(), false, false,
             [this] { rescanFolder(); }},
            {"", true, false, false, nullptr},
            {"Move to Trash", haveTarget, false, false, [this] { moveToTrash(); }},
        },
        x, y);
    redraw();
}

void MainWindow::showApplicationMenu(int x, int y) {
    menuTarget_ = -1;
    menu_.open(
        {
            {"Open\xe2\x80\xa6", showingResults_, false, false, [this] { showWelcome(); }},
            {"Rescan", showingResults_, false, false, [this] { rescan(); }},
            {"", true, false, false, nullptr},
            {"Copy Path", showingResults_, false, false, [this] { copyPath(); }},
            {"Open", showingResults_, false, false, [this] { openSelected(); }},
            {"Show in Files", showingResults_, false, false, [this] { revealSelected(); }},
            {"", true, false, false, nullptr},
            {"Move to Trash", showingResults_, false, false, [this] { moveToTrash(); }},
            {"", true, false, false, nullptr},
            {"Settings\xe2\x80\xa6", true, false, false,
             [this] {
                 settings_.open();
                 redraw();
             }},
            {"About " DISKSPACE_APP_NAME, true, false, false,
             [this] {
                 // Asked on opening rather than only at launch, so opening About
                 // is itself a reason to look - and it costs nothing when the
                 // answer is already known. The other two builds do the same.
                 checkForUpdates();
                 aboutOpen_ = true;
                 redraw();
             }},
        },
        x, y);
    redraw();
}

void MainWindow::showMetricMenu(int x, int y) {
    std::vector<MenuItem> items;
    for (int i = 0; i < 4; ++i) {
        items.push_back({kMetricNames[i], true, int(options_.metric) == i, true,
                         [this, i] { setMetric(SizeMetric(i)); }});
    }
    menu_.open(items, x, y);
    redraw();
}

void MainWindow::rescan() {
    if (!shownRoot_.empty()) scanPath(shownRoot_);
}

void MainWindow::rescanFolder() {
    const int32_t node = actionTarget();
    if (!scan_.scanner() || node <= 0 || scan_.running()) return;
    const diskscan::NodeArena& arena = scan_.scanner()->arena();
    if (!arena.isDirectory(size_t(uint32_t(node)))) return;
    scan_.scanner()->rescan(node);
    refreshViews();
    redraw();
}

void MainWindow::chooseFolder() {
    chooser_.open(shownRoot_.empty() ? std::string() : shownRoot_);
    redraw();
}

// MARK: - Updates

void MainWindow::checkForUpdates() {
    UpdateCheck::shared().check([this](const UpdateAvailable& update) {
        bannerText_ = update.summary();
        bannerUrl_ = update.downloads;
        bannerVisible_ = true;
        redraw();
    });
}

}  // namespace app

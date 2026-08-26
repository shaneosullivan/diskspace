// windows.h first: commctrl.h and shlobj.h both assume its types are already
// defined, and alphabetical order would break the build.
#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>

#include "../core/file_kind.h"
#include "../core/win_util.h"
#include "kind_pane.h"
#include "panels.h"
#include "preferences.h"
#include "preferences_window.h"
#include "resource.h"
#include "scan_session.h"
#include "tree_pane.h"
#include "treemap.h"
#include "treemap_view.h"
#include "ui_common.h"
#include "update_check.h"
#include "welcome_view.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace app {
namespace {

constexpr const wchar_t* kMainClass = L"DiskSpaceMainWindow";
constexpr const wchar_t* kAppTitle = L"Disk Space";
constexpr int kToolbarHeight = 42;
constexpr int kSplitterWidth = 6;
/// The map is what the eye reads first, so the list starts narrow enough to leave
/// the map the larger share. The dividers are the user's from then on.
constexpr int kKindPaneWidth = 300;
constexpr int kTreePaneWidth = 360;

/// Sentinel ids for the two blocks that are not nodes in the tree.
constexpr int32_t kFreeSpaceId = -2;
constexpr int32_t kOtherSpaceId = -3;

class MainWindow {
public:
    void run(const std::wstring& initialPath);

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);

    void buildChrome();
    void buildMenu();
    void layout();
    void paintToolbar(HDC dc);
    void refresh(bool live);
    void showWelcome();
    void scan(const std::wstring& path);
    void chooseFolder();
    void applyPreferences();
    void updateMetricAvailability();
    std::vector<ExtraBlock> extraBlocks() const;
    int64_t displaySize(size_t node) const;
    void updateInspectors(int32_t node);
    void command(int id);
    void showContextMenu(int x, int y);
    void relaunchElevated();
    void offerElevationIfUseful();
    void moveToRecycleBin();
    void showAbout();
    void checkForUpdates();
    bool splitterHitTest(int x, int y, int* which) const;

    HWND window_ = nullptr;
    HWND openButton_ = nullptr;
    HWND metricCombo_ = nullptr;
    HWND zoomOutButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND refreshButton_ = nullptr;
    HACCEL accelerators_ = nullptr;

    KindPane* kindPane_ = nullptr;
    TreePane* treePane_ = nullptr;
    TreemapView* treemap_ = nullptr;
    WelcomeView* welcome_ = nullptr;

    ScanSession session_;
    KindPalette palette_;
    std::wstring status_;
    bool showingWelcome_ = true;

    int kindWidth_ = kKindPaneWidth;
    int treeWidth_ = kTreePaneWidth;
    int draggingSplitter_ = -1;
    int dragOrigin_ = 0;
    int dragStartWidth_ = 0;
    /// Asked at most once per scan, so a refresh does not put the prompt up again.
    bool offeredElevation_ = false;
    ui::BackBuffer toolbarBuffer_;
};

std::wstring browseForFolder(HWND owner) {
    std::wstring result;
    IFileDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    // A disk-space tool has to be able to reach the folders that actually fill up,
    // and a great many of them are hidden.
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_FORCESHOWHIDDEN);
    dialog->SetTitle(L"Choose a folder or drive to scan");
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// MARK: - Chrome

void MainWindow::buildMenu() {
    HMENU menu = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_REFRESH, L"&Refresh Everything\tCtrl+R");
    AppendMenuW(file, MF_STRING, IDM_REFRESH_FOLDER, L"Refresh &Folder");
    AppendMenuW(file, MF_STRING, IDM_STOP, L"&Stop Scanning\tEsc");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_PREFERENCES, L"Se&ttings…\tCtrl+,");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(file), L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_COPY_PATH, L"&Copy Path\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_REVEAL, L"Show in &Explorer");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_RECYCLE, L"Move to Recycle &Bin\tDel");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(edit), L"&Edit");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_ZOOM_IN, L"Zoom &In\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_ZOOM_OUT, L"Zoom &Out\tCtrl+-");
    AppendMenuW(view, MF_STRING, IDM_ZOOM_RESET, L"Zoom to &Top");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_COLLAPSE_ALL, L"&Collapse All Folders");
    AppendMenuW(view, MF_STRING, IDM_SELECT_PARENT, L"Select &Parent Folder");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_KINDS, L"File &Kinds Panel");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_SELECTION, L"&Selection Panel");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_INFORMATION, L"&Information Panel");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_FREE_SPACE, L"Show &Free Space");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_OTHER_SPACE, L"Show O&ther Space on Drive");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_LOGICAL, L"Show &Logical Size");
    AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_CONSISTENT_COLOURS, L"Consistent Kind Colours");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_ANIMATION, L"Animated Zooming");
    AppendMenuW(view, MF_STRING, IDM_TOGGLE_SPLIT, L"Split Horizontally");
    AppendMenuW(view, MF_STRING, IDM_SMALL_FONT_FILES, L"Small Font in File List");
    AppendMenuW(view, MF_STRING, IDM_SMALL_FONT_KINDS, L"Small Font in File Kinds");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(view), L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_RESTART_ELEVATED, L"Restart as &Administrator…");
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, IDM_CHECK_UPDATES, L"Check for &Updates…");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&About Disk Space");
    AppendMenuW(menu, MF_POPUP, UINT_PTR(help), L"&Help");

    SetMenu(window_, menu);
}

void MainWindow::buildChrome() {
    HFONT font = ui::font(window_, ui::FontRole::Body);
    // Owner drawn rather than standard: a stock push button keeps its light face
    // whatever the theme, and in a dark window the toolbar would be the only
    // white rectangles on screen.
    auto button = [&](const wchar_t* text, int id) {
        HWND control = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0,
                                       window_, HMENU(intptr_t(id)), GetModuleHandleW(nullptr),
                                       nullptr);
        SendMessageW(control, WM_SETFONT, WPARAM(font), TRUE);
        return control;
    };
    openButton_ = button(L"Open…", ID_OPEN_BUTTON);
    zoomOutButton_ = button(L"Zoom Out", ID_ZOOM_OUT_BUTTON);
    stopButton_ = button(L"Stop", ID_STOP_BUTTON);
    refreshButton_ = button(L"Refresh", ID_REFRESH_BUTTON);

    metricCombo_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, window_,
                                   HMENU(intptr_t(ID_METRIC_COMBO)), GetModuleHandleW(nullptr),
                                   nullptr);
    SendMessageW(metricCombo_, WM_SETFONT, WPARAM(font), TRUE);
    for (int i = 0; i < diskscan::kSizeMetricCount; ++i) {
        SendMessageW(metricCombo_, CB_ADDSTRING, 0,
                     LPARAM(diskscan::sizeMetricTitle(SizeMetric(i))));
    }
    SendMessageW(metricCombo_, CB_SETCURSEL, 0, 0);

    kindPane_ = KindPane::create(window_, ID_KIND_PANE);
    treePane_ = TreePane::create(window_, ID_TREE_PANE);
    treemap_ = TreemapView::create(window_, ID_TREEMAP_VIEW);
    welcome_ = WelcomeView::create(window_, ID_WELCOME_VIEW);

    // The title bar and the panes' scrollbars are drawn by Windows, not by us, so
    // they need telling which palette to use.
    ui::applySystemTheme(window_);
    for (HWND child : {kindPane_->window(), treePane_->window(), welcome_->window(),
                       metricCombo_}) {
        ui::applySystemTheme(child);
    }

    kindPane_->setPalette(&palette_);
    treePane_->setPalette(&palette_);
    treemap_->setPalette(&palette_);

    treePane_->displaySize = [this](size_t node) { return displaySize(node); };
    treePane_->onSelect = [this](int32_t node) {
        // Walking down the list walks the highlight into smaller parts of the same
        // rectangle, which is what keeps the map usable as a fixed frame of
        // reference.
        treemap_->setHighlightedNode(node);
        updateInspectors(node);
    };
    treePane_->onCollapseAll = [this] { treePane_->collapseToRoot(); };
    treePane_->onToggleSidePanel = [this] {
        Preferences::shared().setShowFileKindStatistics(
            !Preferences::shared().showFileKindStatistics());
    };

    treemap_->onSelect = [this](int32_t node) {
        treePane_->reveal(node);
        treemap_->setHighlightedNode(node);
        updateInspectors(node);
    };
    treemap_->onZoomChanged = [this] { layout(); refresh(false); };
    treemap_->tooltipContent = [this](int32_t node, int32_t aggregated, std::wstring* title,
                                      std::wstring* detail) {
        const NodeArena* arena = session_.arena();
        if (!arena) return false;
        if (node == kFreeSpaceId) {
            *title = L"Free space on this drive";
            *detail = ui::formatBytes(session_.volume().freeBytes);
            return true;
        }
        if (node == kOtherSpaceId) {
            int64_t other = session_.volume().totalBytes - session_.volume().freeBytes -
                            arena->allocated[0];
            if (session_.scanningWholeVolume()) {
                *title = L"Not reached by this scan";
                *detail = ui::formatBytes(other > 0 ? other : 0) +
                          L"  ·  folders that could not be opened, and the file system's own "
                          L"records";
            } else {
                *title = L"Everything else on this drive";
                *detail = ui::formatBytes(other > 0 ? other : 0);
            }
            return true;
        }
        if (node < 0 || size_t(uint32_t(node)) >= arena->count()) return false;
        size_t index = size_t(uint32_t(node));
        *title = arena->path(index);
        *detail = ui::formatBytes(displaySize(index)) + L"  ·  " +
                  diskscan::FileKind::names()[size_t(arena->kind[index])];
        if (aggregated > 0) {
            *detail += L"  ·  with " + ui::formatCount(aggregated - 1) +
                       L" other items too small to draw";
        }
        return true;
    };

    welcome_->onPick = [this](const std::wstring& path) { scan(path); };
    welcome_->onBrowse = [this] { chooseFolder(); };
    welcome_->onRelaunchElevated = [this] { relaunchElevated(); };
    welcome_->onDownloadUpdate = [this](const std::wstring& url) {
        ShellExecuteW(window_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };

    SelectionPanel::shared().onChoose = [this](int32_t node) {
        treePane_->reveal(node);
        treemap_->setHighlightedNode(node);
    };

    Preferences::shared().addObserver([this] { applyPreferences(); });
}

bool MainWindow::splitterHitTest(int x, int y, int* which) const {
    if (showingWelcome_ || !treemap_) return false;
    RECT client;
    GetClientRect(window_, &client);
    int top = ui::scaled(window_, kToolbarHeight);
    if (y < top) return false;
    int splitter = ui::scaled(window_, kSplitterWidth);
    int left = 0;
    if (Preferences::shared().showFileKindStatistics()) {
        left = kindWidth_;
        if (x >= left && x < left + splitter) {
            *which = 0;
            return true;
        }
        left += splitter;
    }
    if (Preferences::shared().splitHorizontally()) {
        // Stacked: the second divider runs horizontally between the list and map.
        int paneTop = top;
        int height = client.bottom - paneTop;
        int treeHeight = (std::min)(treeWidth_, height - splitter - ui::scaled(window_, 80));
        if (y >= paneTop + treeHeight && y < paneTop + treeHeight + splitter && x >= left) {
            *which = 1;
            return true;
        }
        return false;
    }
    left += treeWidth_;
    if (x >= left && x < left + splitter) {
        *which = 1;
        return true;
    }
    return false;
}

void MainWindow::layout() {
    // Creating the window sends a size message before the panes exist.
    if (!treemap_) return;
    RECT client;
    GetClientRect(window_, &client);
    int width = client.right;
    int height = client.bottom;
    int top = ui::scaled(window_, kToolbarHeight);
    int splitter = ui::scaled(window_, kSplitterWidth);

    int pad = ui::scaled(window_, 10);
    int buttonHeight = ui::scaled(window_, 26);
    int buttonY = (top - buttonHeight) / 2;
    int x = pad;
    MoveWindow(openButton_, x, buttonY, ui::scaled(window_, 74), buttonHeight, TRUE);
    x += ui::scaled(window_, 84);
    MoveWindow(metricCombo_, x, buttonY, ui::scaled(window_, 210), ui::scaled(window_, 240), TRUE);

    int right = width - pad;
    int refreshWidth = ui::scaled(window_, 82);
    MoveWindow(refreshButton_, right - refreshWidth, buttonY, refreshWidth, buttonHeight, TRUE);
    right -= refreshWidth + ui::scaled(window_, 8);
    int stopWidth = ui::scaled(window_, 68);
    MoveWindow(stopButton_, right - stopWidth, buttonY, stopWidth, buttonHeight, TRUE);
    right -= stopWidth + ui::scaled(window_, 8);
    int zoomWidth = ui::scaled(window_, 92);
    MoveWindow(zoomOutButton_, right - zoomWidth, buttonY, zoomWidth, buttonHeight, TRUE);

    if (showingWelcome_) {
        MoveWindow(welcome_->window(), 0, top, width, height - top, TRUE);
        ShowWindow(welcome_->window(), SW_SHOW);
        ShowWindow(kindPane_->window(), SW_HIDE);
        ShowWindow(treePane_->window(), SW_HIDE);
        ShowWindow(treemap_->window(), SW_HIDE);
        return;
    }
    ShowWindow(welcome_->window(), SW_HIDE);

    int left = 0;
    const bool showKinds = Preferences::shared().showFileKindStatistics();
    if (showKinds) {
        kindWidth_ = (std::max)(ui::scaled(window_, 140),
                                (std::min)(kindWidth_, width - ui::scaled(window_, 400)));
        MoveWindow(kindPane_->window(), 0, top, kindWidth_, height - top, TRUE);
        ShowWindow(kindPane_->window(), SW_SHOW);
        left = kindWidth_ + splitter;
    } else {
        ShowWindow(kindPane_->window(), SW_HIDE);
    }

    if (Preferences::shared().splitHorizontally()) {
        int available = height - top;
        int treeHeight = (std::max)(ui::scaled(window_, 120),
                                    (std::min)(treeWidth_, available - ui::scaled(window_, 140)));
        MoveWindow(treePane_->window(), left, top, width - left, treeHeight, TRUE);
        MoveWindow(treemap_->window(), left, top + treeHeight + splitter, width - left,
                   available - treeHeight - splitter, TRUE);
    } else {
        treeWidth_ = (std::max)(ui::scaled(window_, 200),
                                (std::min)(treeWidth_, width - left - ui::scaled(window_, 200)));
        MoveWindow(treePane_->window(), left, top, treeWidth_, height - top, TRUE);
        int mapLeft = left + treeWidth_ + splitter;
        MoveWindow(treemap_->window(), mapLeft, top, (std::max)(0, width - mapLeft), height - top,
                   TRUE);
    }
    ShowWindow(treePane_->window(), SW_SHOW);
    ShowWindow(treemap_->window(), SW_SHOW);
}

void MainWindow::paintToolbar(HDC target) {
    RECT client;
    GetClientRect(window_, &client);
    int height = ui::scaled(window_, kToolbarHeight);
    HDC dc = toolbarBuffer_.begin(target, client.right, height);
    if (!dc || !metricCombo_) return;
    const ui::Theme& t = ui::theme();
    RECT bar{0, 0, client.right, height};
    ui::fill(dc, bar, t.windowBackground);
    RECT edge{0, height - 1, client.right, height};
    ui::fill(dc, edge, t.separator);

    if (!showingWelcome_) {
        RECT statusRect;
        GetWindowRect(metricCombo_, &statusRect);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&statusRect), 2);
        RECT zoomRect;
        GetWindowRect(zoomOutButton_, &zoomRect);
        MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&zoomRect), 2);
        RECT text{statusRect.right + ui::scaled(window_, 14), 0,
                  zoomRect.left - ui::scaled(window_, 10), height - 1};
        if (text.right > text.left) {
            ui::drawText(dc, status_, text, t.secondaryText,
                         ui::font(window_, ui::FontRole::Body),
                         DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
    toolbarBuffer_.blit(target, client.right, height);
}

// MARK: - Scanning

void MainWindow::scan(const std::wstring& path) {
    InformationPanel::shared().clear();
    SelectionPanel::shared().clear();
    SetWindowTextW(window_, (std::wstring(kAppTitle) + L" — " + path).c_str());
    showingWelcome_ = false;
    offeredElevation_ = false;
    treemap_->setArena(nullptr);
    treemap_->resetLivePacing();
    treePane_->resetExpansion();
    EnableWindow(stopButton_, TRUE);
    SetWindowTextW(stopButton_, L"Stop");

    session_.start(path);
    kindPane_->setScanner(session_.scanner());
    treePane_->setArena(session_.arena(), session_.rootPath());
    treemap_->setArena(session_.arena());
    layout();
    refresh(false);
}

void MainWindow::showWelcome() {
    if (showingWelcome_) return;
    // Nothing leads back to the results from here, so a scan still running would
    // be publishing into views no one can reach.
    session_.cancel();
    kindPane_->setScanner(nullptr);
    treePane_->setArena(nullptr, L"");
    treemap_->setArena(nullptr);
    InformationPanel::shared().clear();
    SelectionPanel::shared().clear();
    SetWindowTextW(window_, kAppTitle);
    showingWelcome_ = true;
    welcome_->reload();
    layout();
    refresh(false);
}

void MainWindow::chooseFolder() {
    std::wstring path = browseForFolder(window_);
    if (!path.empty()) scan(path);
}

std::vector<ExtraBlock> MainWindow::extraBlocks() const {
    std::vector<ExtraBlock> blocks;
    const diskscan::VolumeInfo& volume = session_.volume();
    if (volume.totalBytes <= 0) return blocks;
    const NodeArena* arena = session_.arena();
    if (!arena) return blocks;

    if (Preferences::shared().showFreeSpace()) {
        ExtraBlock block;
        block.id = kFreeSpaceId;
        block.label = L"Free space";
        block.bytes = volume.freeBytes;
        block.colour = ui::theme().dark ? 0xFF2E2E32 : 0xFFD8D8DE;
        blocks.push_back(block);
    }
    // Shown even for a whole drive, unlike the macOS version, because on Windows
    // there genuinely is something else: directories closed even to an
    // administrator, and the file system's own metadata, which a directory walk
    // cannot see at all. Hiding it would leave the difference between this total
    // and the drive's used space unexplained.
    if (Preferences::shared().showOtherSpace()) {
        int64_t other = volume.totalBytes - volume.freeBytes - arena->allocated[0];
        if (other > 0) {
            ExtraBlock block;
            block.id = kOtherSpaceId;
            block.label = session_.scanningWholeVolume() ? L"Not reached by this scan"
                                                        : L"Other space on this drive";
            block.bytes = other;
            block.colour = 0xFF4A4A52;
            blocks.push_back(block);
        }
    }
    return blocks;
}

int64_t MainWindow::displaySize(size_t node) const {
    const NodeArena* arena = session_.arena();
    if (!arena || node >= arena->count()) return 0;
    // Only the total has both forms; the recency figures are accumulated from
    // occupied space alone.
    if (!Preferences::shared().showPhysicalFileSize() && session_.metric == SizeMetric::Total) {
        return arena->logical[node];
    }
    return arena->size(session_.metric, node);
}

void MainWindow::updateInspectors(int32_t node) {
    const NodeArena* arena = session_.arena();
    if (!arena || node < 0 || size_t(uint32_t(node)) >= arena->count()) return;
    size_t index = size_t(uint32_t(node));
    if (InformationPanel::shared().visible()) {
        InformationPanel::shared().update(*arena, index, ui::formatBytes(displaySize(index)));
    }
    if (SelectionPanel::shared().visible()) {
        SelectionPanel::shared().update(*arena, node, [this](size_t n) {
            return ui::formatBytes(displaySize(n));
        });
    }
}

void MainWindow::updateMetricAvailability() {
    // "Added Since Last Scan" is meaningless without a baseline; say so rather
    // than silently showing zeros.
    bool hasBaseline = session_.sinceLastScanBaseline() != 0;
    int index = int(SizeMetric::AddedSinceLastScan);
    std::wstring title = diskscan::sizeMetricTitle(SizeMetric::AddedSinceLastScan);
    if (!hasBaseline) title += L" (no previous scan)";
    int selected = int(SendMessageW(metricCombo_, CB_GETCURSEL, 0, 0));
    wchar_t existing[128] = {};
    SendMessageW(metricCombo_, CB_GETLBTEXT, WPARAM(index), LPARAM(existing));
    if (title != existing) {
        SendMessageW(metricCombo_, CB_DELETESTRING, WPARAM(index), 0);
        SendMessageW(metricCombo_, CB_INSERTSTRING, WPARAM(index), LPARAM(title.c_str()));
        SendMessageW(metricCombo_, CB_SETCURSEL, WPARAM(selected), 0);
    }
}

void MainWindow::refresh(bool live) {
    if (!treemap_) return;
    const NodeArena* arena = session_.arena();

    if (showingWelcome_) {
        ShowWindow(openButton_, SW_HIDE);
        ShowWindow(metricCombo_, SW_HIDE);
        ShowWindow(zoomOutButton_, SW_HIDE);
        ShowWindow(stopButton_, SW_HIDE);
        ShowWindow(refreshButton_, SW_HIDE);
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    ShowWindow(openButton_, SW_SHOW);
    ShowWindow(metricCombo_, SW_SHOW);
    ShowWindow(refreshButton_, SW_SHOW);

    updateMetricAvailability();

    if (!Preferences::shared().consistentKindColours() && session_.scanner()) {
        auto totals = session_.scanner()->kindTotals(session_.metric);
        std::sort(totals.begin(), totals.end(),
                  [](const auto& a, const auto& b) { return a.bytes > b.bytes; });
        std::vector<int16_t> ranking;
        ranking.reserve(totals.size());
        for (const auto& total : totals) ranking.push_back(total.kind);
        palette_.setRanking(ranking);
    }

    treePane_->setGeneration(session_.generation());
    kindPane_->refresh();
    treePane_->refresh();
    if (live) {
        treemap_->liveRefresh();
    } else {
        treemap_->refresh();
    }

    ShowWindow(zoomOutButton_, treemap_->canZoomOut() ? SW_SHOW : SW_HIDE);
    ShowWindow(stopButton_, session_.isScanning() ? SW_SHOW : SW_HIDE);
    EnableWindow(refreshButton_, !session_.isScanning());

    if (!arena || arena->count() == 0) {
        status_ = session_.isScanning() ? L"Starting scan…" : L"";
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    int64_t files = arena->fileCount[0];
    int64_t nodes = int64_t(arena->count());
    std::wstring counts = ui::formatCount(files) + L" files · " +
                          ui::formatCount(nodes - files) + L" folders · " +
                          ui::formatBytes(arena->allocated[0]);
    if (session_.isScanning()) {
        status_ = L"Scanning… " + counts + L" · " + ui::formatSeconds(session_.elapsed(), false);
    } else {
        status_ = (session_.wasStopped() ? L"Scan stopped · " : L"Scan complete · ") + counts +
                  L" · " + ui::formatSeconds(session_.elapsed(), false);
        if (session_.metric != SizeMetric::Total) {
            status_ += std::wstring(L"  —  ") + diskscan::sizeMetricTitle(session_.metric) + L": " +
                       ui::formatBytes(arena->size(session_.metric, 0));
        }
        if (session_.backendUsed() == ScanBackend::Mft) status_ += L"  ·  read from the file table";
    }
    InvalidateRect(window_, nullptr, FALSE);
}

void MainWindow::applyPreferences() {
    palette_.consistentColours = Preferences::shared().consistentKindColours();
    kindPane_->setUseLogicalSize(!Preferences::shared().showPhysicalFileSize());
    treePane_->sidePanelVisible = Preferences::shared().showFileKindStatistics();
    TreemapOptions options;
    options.metric = session_.metric;
    options.useLogicalSize = !Preferences::shared().showPhysicalFileSize();
    options.extraBlocks = extraBlocks();
    treemap_->setOptions(options);
    layout();
    refresh(false);
}

// MARK: - Elevation

void MainWindow::relaunchElevated() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring arguments;
    if (!session_.rootPath().empty()) arguments = L"\"" + session_.rootPath() + L"\"";

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.lpVerb = L"runas";
    info.lpFile = path;
    info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info)) {
        // The new process takes over, including the folder that was on screen.
        PostMessageW(window_, WM_CLOSE, 0, 0);
    }
    // Declining the prompt is an answer, not an error, so nothing is reported.
}

void MainWindow::offerElevationIfUseful() {
    if (offeredElevation_ || !Preferences::shared().offerElevation()) return;
    if (!session_.elevationWouldHelp() || !session_.scanningWholeVolume()) return;
    offeredElevation_ = true;

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = window_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = kAppTitle;
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"This drive can be scanned much faster";
    config.pszContent =
        L"Running as administrator lets Disk Space read the drive's file table directly, "
        L"which scans a whole drive in seconds rather than minutes. The figures are the "
        L"same either way.";
    config.pszVerificationText = L"Don't offer this again";
    TASKDIALOG_BUTTON buttons[] = {
        {IDYES, L"Restart as administrator\nThe same folder is scanned again straight away."},
        {IDNO, L"Keep the results\nNothing is rescanned."},
    };
    config.pButtons = buttons;
    config.cButtons = ARRAYSIZE(buttons);
    config.dwFlags |= TDF_USE_COMMAND_LINKS;
    config.nDefaultButton = IDNO;

    int pressed = IDNO;
    BOOL verified = FALSE;
    if (SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, &verified))) {
        if (verified) Preferences::shared().setOfferElevation(false);
        if (pressed == IDYES) relaunchElevated();
    }
}

// MARK: - Commands

void MainWindow::moveToRecycleBin() {
    const NodeArena* arena = session_.arena();
    int32_t node = treePane_->selectedNode();
    if (!arena || node < 0 || size_t(uint32_t(node)) >= arena->count()) return;
    std::wstring path = arena->path(size_t(uint32_t(node)));

    std::wstring message = L"Move this to the Recycle Bin?\n\n" + path;
    if (MessageBoxW(window_, message.c_str(), kAppTitle, MB_OKCANCEL | MB_ICONWARNING) != IDOK) {
        return;
    }

    // SHFileOperation wants the source double-NUL terminated.
    std::wstring source = path;
    source.push_back(L'\0');
    source.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.hwnd = window_;
    operation.wFunc = FO_DELETE;
    operation.pFrom = source.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int result = SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        MessageBoxW(window_, L"That could not be moved to the Recycle Bin.", kAppTitle,
                    MB_OK | MB_ICONERROR);
        return;
    }
    // The bytes are gone from the disk, so they go out of every figure that
    // counted them. The row itself stays, showing zero.
    session_.removeNode(node);
    treemap_->setHighlightedNode(-1);
    refresh(false);
    updateInspectors(node);
}

void MainWindow::showAbout() {
    // Asked on every open rather than only at launch, so opening About is itself
    // a reason to look — and it costs nothing when the answer is already known.
    UpdateCheck::shared().check(window_);
    const UpdateAvailable* update = UpdateCheck::shared().result();

    std::wstring content =
        L"Find out where your disk space went. Scans a folder or a whole drive in parallel and "
        L"shows the results as they arrive, with a treemap of every file and four measures of "
        L"size — including what has only appeared recently.\n\n"
        L"Built by Chofter Apps · diskspace.io";

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = window_;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"About Disk Space";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Disk Space";
    std::wstring version = L"Version " + UpdateCheck::currentVersion();
    config.pszContent = content.c_str();
    config.pszFooter = version.c_str();

    std::wstring downloadLabel;
    TASKDIALOG_BUTTON buttons[1];
    if (update) {
        downloadLabel = update->summary() + L"\nOpens diskspace.io in your browser.";
        buttons[0] = {IDYES, downloadLabel.c_str()};
        config.pButtons = buttons;
        config.cButtons = 1;
        config.dwFlags |= TDF_USE_COMMAND_LINKS;
    }
    config.dwCommonButtons = TDCBF_CLOSE_BUTTON;

    int pressed = 0;
    if (SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDYES &&
        update) {
        ShellExecuteW(window_, L"open", update->downloads.c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
    }
}

/// The Help menu's own entry, which must say something either way: a check that
/// silently does nothing reads as broken.
void MainWindow::checkForUpdates() {
    UpdateCheck& check = UpdateCheck::shared();
    check.check(window_);
    if (!check.finished()) {
        MessageBoxW(window_, L"Checking diskspace.io for a newer version…", kAppTitle,
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (const UpdateAvailable* update = check.result()) {
        std::wstring message = update->summary() + L"\n\nOpen the downloads page?";
        if (MessageBoxW(window_, message.c_str(), kAppTitle, MB_YESNO | MB_ICONINFORMATION) ==
            IDYES) {
            ShellExecuteW(window_, L"open", update->downloads.c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        }
        return;
    }
    MessageBoxW(window_,
                (L"Version " + UpdateCheck::currentVersion() + L" is the latest release.").c_str(),
                kAppTitle, MB_OK | MB_ICONINFORMATION);
}

void MainWindow::showContextMenu(int x, int y) {
    if (showingWelcome_) return;
    const NodeArena* arena = session_.arena();
    int32_t node = treePane_->selectedNode();
    if (!arena || node < 0) return;

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_REVEAL, L"Show in Explorer");
    AppendMenuW(menu, MF_STRING, IDM_COPY_PATH, L"Copy Path");
    if (arena->isDirectory(size_t(uint32_t(node)))) {
        AppendMenuW(menu, MF_STRING, IDM_ZOOM_IN, L"Zoom Treemap Here");
        AppendMenuW(menu, MF_STRING | (session_.isScanning() ? MF_GRAYED : 0), IDM_REFRESH_FOLDER,
                    L"Refresh This Folder");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (session_.isScanning() ? MF_GRAYED : 0), IDM_RECYCLE,
                L"Move to Recycle Bin…");
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, x, y, 0, window_, nullptr);
    DestroyMenu(menu);
}

void MainWindow::command(int id) {
    const NodeArena* arena = session_.arena();
    int32_t selected = treePane_ ? treePane_->selectedNode() : -1;
    Preferences& preferences = Preferences::shared();

    switch (id) {
        case IDM_OPEN:
        case ID_OPEN_BUTTON:
            showWelcome();
            return;
        case IDM_REFRESH:
        case ID_REFRESH_BUTTON:
            if (!session_.isScanning() && !session_.rootPath().empty()) scan(session_.rootPath());
            return;
        case IDM_REFRESH_FOLDER:
            if (!session_.isScanning() && arena && selected >= 0 &&
                arena->isDirectory(size_t(uint32_t(selected)))) {
                treemap_->setHighlightedNode(-1);
                treePane_->resetExpansion();
                session_.refreshFolder(selected);
                refresh(false);
            }
            return;
        case IDM_STOP:
        case ID_STOP_BUTTON:
            if (session_.isScanning()) {
                session_.stop();
                EnableWindow(stopButton_, FALSE);
                SetWindowTextW(stopButton_, L"Stopping…");
            }
            return;
        case IDM_EXIT:
            PostMessageW(window_, WM_CLOSE, 0, 0);
            return;

        case IDM_COPY_PATH: {
            if (!arena || selected < 0) return;
            std::wstring path = arena->path(size_t(uint32_t(selected)));
            if (!OpenClipboard(window_)) return;
            EmptyClipboard();
            size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory) {
                memcpy(GlobalLock(memory), path.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            }
            CloseClipboard();
            return;
        }
        case IDM_REVEAL: {
            if (!arena || selected < 0) return;
            std::wstring path = arena->path(size_t(uint32_t(selected)));
            std::wstring arguments = L"/select,\"" + path + L"\"";
            ShellExecuteW(window_, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                          SW_SHOWNORMAL);
            return;
        }
        case IDM_RECYCLE:
            moveToRecycleBin();
            return;

        case IDM_ZOOM_IN:
            if (arena && selected >= 0) {
                int32_t target = selected;
                if (!arena->isDirectory(size_t(uint32_t(target)))) {
                    target = arena->parent[size_t(uint32_t(target))];
                }
                treemap_->zoomTo(target);
            }
            return;
        case IDM_ZOOM_OUT:
        case ID_ZOOM_OUT_BUTTON:
            treemap_->zoomOut();
            return;
        case IDM_ZOOM_RESET:
            treemap_->resetZoom();
            return;
        case IDM_COLLAPSE_ALL:
            treePane_->collapseToRoot();
            return;
        case IDM_SELECT_PARENT:
            if (arena && selected > 0) {
                int32_t parent = arena->parent[size_t(uint32_t(selected))];
                treePane_->reveal(parent);
                treemap_->setHighlightedNode(parent);
            }
            return;

        case IDM_TOGGLE_KINDS:
            preferences.setShowFileKindStatistics(!preferences.showFileKindStatistics());
            return;
        case IDM_TOGGLE_SELECTION:
            if (SelectionPanel::shared().visible()) {
                SelectionPanel::shared().hide();
            } else {
                SelectionPanel::shared().show(window_);
                updateInspectors(selected);
            }
            return;
        case IDM_TOGGLE_INFORMATION:
            if (InformationPanel::shared().visible()) {
                InformationPanel::shared().hide();
            } else {
                InformationPanel::shared().show(window_);
                updateInspectors(selected);
            }
            return;
        case IDM_TOGGLE_FREE_SPACE:
            preferences.setShowFreeSpace(!preferences.showFreeSpace());
            return;
        case IDM_TOGGLE_OTHER_SPACE:
            preferences.setShowOtherSpace(!preferences.showOtherSpace());
            return;
        case IDM_TOGGLE_LOGICAL:
            preferences.setShowPhysicalFileSize(!preferences.showPhysicalFileSize());
            return;
        case IDM_TOGGLE_CONSISTENT_COLOURS:
            preferences.setConsistentKindColours(!preferences.consistentKindColours());
            return;
        case IDM_TOGGLE_ANIMATION:
            preferences.setAnimatedZooming(!preferences.animatedZooming());
            return;
        case IDM_TOGGLE_SPLIT:
            preferences.setSplitHorizontally(!preferences.splitHorizontally());
            return;
        case IDM_SMALL_FONT_FILES:
            preferences.setSmallFontFiles(!preferences.smallFontFiles());
            return;
        case IDM_SMALL_FONT_KINDS:
            preferences.setSmallFontKinds(!preferences.smallFontKinds());
            return;

        case IDM_PREFERENCES:
            PreferencesWindow::shared().show(window_);
            return;
        case IDM_RESTART_ELEVATED:
            relaunchElevated();
            return;
        case IDM_CHECK_UPDATES:
            checkForUpdates();
            return;
        case IDM_ABOUT:
            showAbout();
            return;
        default:
            break;
    }
}

// MARK: - Window

LRESULT CALLBACK MainWindow::proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(window, GWLP_USERDATA, LONG_PTR(create->lpCreateParams));
        reinterpret_cast<MainWindow*>(create->lpCreateParams)->window_ = window;
    }
    auto* main = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!main) return DefWindowProcW(window, message, w, l);
    return main->handle(message, w, l);
}

LRESULT MainWindow::handle(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            paintToolbar(dc);
            if (treemap_) {
                // Whatever no pane covers, which is the splitter tracks.
                RECT client;
                GetClientRect(window_, &client);
                client.top = ui::scaled(window_, kToolbarHeight);
                HRGN region = CreateRectRgnIndirect(&client);
                for (HWND child : {kindPane_->window(), treePane_->window(), treemap_->window(),
                                   welcome_->window()}) {
                    if (!IsWindowVisible(child)) continue;
                    RECT bounds;
                    GetWindowRect(child, &bounds);
                    MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
                    HRGN cut = CreateRectRgnIndirect(&bounds);
                    CombineRgn(region, region, cut, RGN_DIFF);
                    DeleteObject(cut);
                }
                HBRUSH brush = CreateSolidBrush(ui::theme().separator);
                FillRgn(dc, region, brush);
                DeleteObject(brush);
                DeleteObject(region);
            }
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_DPICHANGED: {
            auto* suggested = reinterpret_cast<RECT*>(l);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            HFONT font = ui::font(window_, ui::FontRole::Body);
            for (HWND control : {openButton_, metricCombo_, zoomOutButton_, stopButton_,
                                 refreshButton_}) {
                SendMessageW(control, WM_SETFONT, WPARAM(font), TRUE);
            }
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            // The theme lives in the same place as the rest of the personalisation
            // settings, so this fires when light and dark are switched.
            if (ui::refreshTheme()) {
                ui::applySystemTheme(window_);
                if (treemap_) {
                    for (HWND child : {kindPane_->window(), treePane_->window(),
                                       welcome_->window(), metricCombo_}) {
                        ui::applySystemTheme(child);
                    }
                    treemap_->refresh();
                }
                InvalidateRect(window_, nullptr, TRUE);
            }
            return 0;
        case WM_TIMER:
            if (session_.isScanning()) {
                // Every tick is a new generation, which is what makes the tree
                // resort. Without it the list keeps whichever order the folders
                // happened to be in on the first tick, and a folder that turns
                // out to be the largest never climbs.
                session_.bumpGeneration();
                refresh(true);
            }
            return 0;
        case UpdateCheck::kMessage:
            // The chooser is where a new version is worth mentioning: it is the
            // one screen the user is not in the middle of reading something on.
            if (welcome_) welcome_->setUpdate(UpdateCheck::shared().result());
            return 0;
        case ScanSession::kRefreshMessage:
            refresh(false);
            SetWindowTextW(stopButton_, L"Stop");
            EnableWindow(stopButton_, TRUE);
            offerElevationIfUseful();
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(w);
            if (id == ID_METRIC_COMBO && HIWORD(w) == CBN_SELCHANGE) {
                int index = int(SendMessageW(metricCombo_, CB_GETCURSEL, 0, 0));
                if (index < 0 || index >= diskscan::kSizeMetricCount) return 0;
                if (SizeMetric(index) == SizeMetric::AddedSinceLastScan &&
                    session_.sinceLastScanBaseline() == 0) {
                    // Nothing to compare against, so the choice is refused rather
                    // than answered with zeros.
                    SendMessageW(metricCombo_, CB_SETCURSEL, WPARAM(int(session_.metric)), 0);
                    return 0;
                }
                session_.metric = SizeMetric(index);
                treemap_->setMetric(session_.metric);
                treePane_->setMetric(session_.metric);
                kindPane_->setMetric(session_.metric);
                // Both views start over: a drill-down that made sense for total
                // size is rarely where the recently-added bytes are.
                treemap_->resetZoom();
                treemap_->setHighlightedNode(-1);
                treePane_->collapseToRoot();
                treePane_->setSizeColumnTitle(SizeMetric(index) == SizeMetric::Total
                                                  ? L"Size"
                                                  : diskscan::sizeMetricTitle(SizeMetric(index)));
                refresh(false);
                return 0;
            }
            command(id);
            return 0;
        }
        case WM_INITMENUPOPUP: {
            HMENU menu = HMENU(w);
            Preferences& p = Preferences::shared();
            auto check = [&](int id, bool on) {
                CheckMenuItem(menu, UINT(id), MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            };
            check(IDM_TOGGLE_KINDS, p.showFileKindStatistics());
            check(IDM_TOGGLE_SELECTION, SelectionPanel::shared().visible());
            check(IDM_TOGGLE_INFORMATION, InformationPanel::shared().visible());
            check(IDM_TOGGLE_FREE_SPACE, p.showFreeSpace());
            check(IDM_TOGGLE_OTHER_SPACE, p.showOtherSpace());
            check(IDM_TOGGLE_LOGICAL, !p.showPhysicalFileSize());
            check(IDM_TOGGLE_CONSISTENT_COLOURS, p.consistentKindColours());
            check(IDM_TOGGLE_ANIMATION, p.animatedZooming());
            check(IDM_TOGGLE_SPLIT, p.splitHorizontally());
            check(IDM_SMALL_FONT_FILES, p.smallFontFiles());
            check(IDM_SMALL_FONT_KINDS, p.smallFontKinds());
            EnableMenuItem(menu, IDM_RESTART_ELEVATED,
                           MF_BYCOMMAND | (diskscan::isElevated() ? MF_GRAYED : MF_ENABLED));
            EnableMenuItem(menu, IDM_ZOOM_OUT,
                           MF_BYCOMMAND | (treemap_->canZoomOut() ? MF_ENABLED : MF_GRAYED));
            return 0;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (item->CtlType == ODT_BUTTON) {
                wchar_t text[64] = {};
                GetWindowTextW(item->hwndItem, text, 64);
                ui::drawOwnerDrawnButton(*item, text, ui::font(window_, ui::FontRole::Body));
                return TRUE;
            }
            return DefWindowProcW(window_, message, w, l);
        }
        case WM_CONTEXTMENU:
            showContextMenu(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_SETCURSOR: {
            POINT cursor;
            GetCursorPos(&cursor);
            ScreenToClient(window_, &cursor);
            int which;
            if (HWND(w) == window_ && splitterHitTest(cursor.x, cursor.y, &which)) {
                SetCursor(LoadCursorW(nullptr,
                                      (which == 1 && Preferences::shared().splitHorizontally())
                                          ? IDC_SIZENS
                                          : IDC_SIZEWE));
                return 1;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            int which;
            if (splitterHitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l), &which)) {
                draggingSplitter_ = which;
                bool horizontal = which == 1 && Preferences::shared().splitHorizontally();
                dragOrigin_ = horizontal ? GET_Y_LPARAM(l) : GET_X_LPARAM(l);
                dragStartWidth_ = which == 0 ? kindWidth_ : treeWidth_;
                SetCapture(window_);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (draggingSplitter_ < 0) return 0;
            bool horizontal = draggingSplitter_ == 1 && Preferences::shared().splitHorizontally();
            int now = horizontal ? GET_Y_LPARAM(l) : GET_X_LPARAM(l);
            int delta = now - dragOrigin_;
            if (draggingSplitter_ == 0) {
                kindWidth_ = dragStartWidth_ + delta;
            } else {
                treeWidth_ = dragStartWidth_ + delta;
            }
            layout();
            return 0;
        }
        case WM_LBUTTONUP:
            if (draggingSplitter_ >= 0) {
                draggingSplitter_ = -1;
                ReleaseCapture();
            }
            return 0;
        case WM_CLOSE:
            session_.cancel();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, w, l);
}

void MainWindow::run(const std::wstring& initialPath) {
    ui::registerClass(kMainClass, proc);
    // Opens at this size and stays there. Resizing on the first scan means the app
    // moves under the user at the moment they are reading it.
    window_ = CreateWindowExW(0, kMainClass, kAppTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                              CW_USEDEFAULT, 1400, 900, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (!window_) return;

    // Both sizes explicitly: the small one is the title bar and the Alt-Tab
    // strip, the large one is the task switcher and the taskbar.
    HINSTANCE instance = GetModuleHandleW(nullptr);
    // Not named `small`: rpcndr.h, which windows.h drags in, defines that as char.
    HANDLE smallIcon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    HANDLE largeIcon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    if (smallIcon) SendMessageW(window_, WM_SETICON, ICON_SMALL, LPARAM(smallIcon));
    if (largeIcon) SendMessageW(window_, WM_SETICON, ICON_BIG, LPARAM(largeIcon));

    buildChrome();
    buildMenu();
    session_.attach(window_);
    accelerators_ = LoadAcceleratorsW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDR_ACCELERATORS));

    applyPreferences();
    welcome_->reload();
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);

    // Asked once at launch, and only if the user has left it on. The answer is
    // cached, so opening About later costs nothing and shows the same thing.
    if (Preferences::shared().checkForUpdates()) UpdateCheck::shared().check(window_);

    if (!initialPath.empty()) scan(initialPath);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (accelerators_ && TranslateAcceleratorW(window_, accelerators_, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace

/// The window class itself stays file-local; only starting it is public.
void runApplication(const std::wstring& initialPath) {
    MainWindow window;
    window.run(initialPath);
}

}  // namespace app

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof(INITCOMMONCONTROLSEX), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    // Before any window exists, so the common controls are created with the right
    // palette rather than re-themed afterwards.
    ui::initialiseAppTheme();
    diskscan::FileKind::warmUp();

    std::wstring initialPath;
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments && count > 1) initialPath = arguments[1];
    if (arguments) LocalFree(arguments);

    app::runApplication(initialPath);

    CoUninitialize();
    return 0;
}

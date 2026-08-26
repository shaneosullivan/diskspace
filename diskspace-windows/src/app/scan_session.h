#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../core/tree_scanner.h"
#include "../core/win_util.h"

namespace app {

using diskscan::NodeArena;
using diskscan::ScanBackend;
using diskscan::ScanResult;
using diskscan::SizeMetric;
using diskscan::TreeScanner;

/// Owns a scan and the derived state the UI reads.
///
/// The scan runs entirely off the UI thread. The UI never blocks on it: it reads
/// the arena's published node count and whatever totals are current, which are
/// always self-consistent enough to draw. Nothing here takes a lock to read.
class ScanSession {
public:
    ~ScanSession();

    /// Posted to the owning window whenever the UI should redraw. `wParam` is
    /// non-zero for the periodic tick during a scan, which is what lets the
    /// treemap pace itself.
    static constexpr UINT kRefreshMessage = WM_APP + 1;
    void attach(HWND window) { window_ = window; }

    void start(const std::wstring& path, ScanBackend backend = ScanBackend::Auto);
    /// Rescans one folder in place, keeping the rest of the tree as it stands.
    void refreshFolder(int32_t node);
    /// Takes an entry out of the totals, for something just sent to the recycle
    /// bin. The row stays, at zero.
    void removeNode(int32_t node);
    /// Stops the walk but keeps everything found so far on screen.
    void stop();
    /// Stops the walk and throws away the results with it.
    void cancel();

    TreeScanner* scanner() const { return scanner_.get(); }
    const NodeArena* arena() const { return scanner_ ? &scanner_->arena() : nullptr; }
    const std::wstring& rootPath() const { return rootPath_; }
    bool isScanning() const { return scanning_.load(); }
    bool wasStopped() const { return wasStopped_; }
    double elapsed() const;
    /// Zero when this root has never been scanned before.
    uint32_t sinceLastScanBaseline() const { return baseline_; }
    /// Bumped whenever the UI should consider its cached derivations stale.
    int generation() const { return generation_; }
    void bumpGeneration() { ++generation_; }

    SizeMetric metric = SizeMetric::Total;

    const diskscan::VolumeInfo& volume() const { return volume_; }
    bool scanningWholeVolume() const { return wholeVolume_; }
    /// Set when the fast backend could have been used but was not, so the window
    /// can offer to relaunch with the rights it needs.
    bool elevationWouldHelp() const { return elevationWouldHelp_; }
    ScanBackend backendUsed() const { return backendUsed_; }

private:
    void startTimer();
    void stopTimer();
    void finish(const ScanResult& result);
    void joinWorker();

    std::unique_ptr<TreeScanner> scanner_;
    std::thread worker_;
    std::wstring rootPath_;
    std::atomic<bool> scanning_{false};
    bool wasStopped_ = false;
    uint32_t baseline_ = 0;
    int generation_ = 0;
    ULONGLONG startedAt_ = 0;
    double finalElapsed_ = 0;
    HWND window_ = nullptr;
    UINT_PTR timer_ = 0;
    diskscan::VolumeInfo volume_;
    bool wholeVolume_ = false;
    bool elevationWouldHelp_ = false;
    ScanBackend backendUsed_ = ScanBackend::Enum;
    /// Guards the generation counter that the scan thread bumps on completion
    /// while the UI thread reads it. Everything else the UI reads is either
    /// atomic or only written before the thread starts.
    std::atomic<int> pendingGeneration_{0};
};

}  // namespace app

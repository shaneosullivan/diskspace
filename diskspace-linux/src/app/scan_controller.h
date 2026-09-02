#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "tree_scanner.h"

namespace app {

/// Runs a scan on a thread of its own and reports back on the main loop.
///
/// The arena is built for concurrent reading while it fills - one applier
/// writing, any number of readers below the published count - so the UI does not
/// wait for the scan and takes no lock to draw. What it does need is a nudge to
/// redraw, and that is `poll`: the window calls it from its own timer, rather
/// than the scan thread calling back, so the X connection is only ever touched
/// from the thread that owns it.
class ScanController {
public:
    ScanController();
    ~ScanController();

    ScanController(const ScanController&) = delete;
    ScanController& operator=(const ScanController&) = delete;

    /// Called on the main loop about ten times a second while a scan runs, and
    /// once more when it finishes.
    std::function<void()> onProgress;
    /// Called on the main loop when the scan has finished or been stopped.
    std::function<void()> onFinished;

    void start(const diskscan::NameString& root);
    void stop();

    /// Called from the window's timer, ten times a second or so. Fires
    /// onProgress, and onFinished once when the scan ends.
    void poll();
    bool running() const { return running_.load(std::memory_order_acquire); }

    diskscan::TreeScanner* scanner() { return scanner_.get(); }
    const diskscan::TreeScanner* scanner() const { return scanner_.get(); }
    const diskscan::ScanResult& result() const { return result_; }
    const diskscan::NameString& root() const { return root_; }

private:
    void join();

    std::unique_ptr<diskscan::TreeScanner> scanner_;
    std::thread thread_;
    diskscan::ScanResult result_;
    diskscan::NameString root_;
    std::atomic<bool> running_{false};
    /// Set by the scan thread, read by the timer, so the finish is announced from
    /// the main loop rather than from the thread that happened to end.
    std::atomic<bool> finished_{false};
    /// Whether a finished scan has already been announced, so the window's timer
    /// can keep running without the end being reported twice.
    bool announced_ = true;
};

}  // namespace app

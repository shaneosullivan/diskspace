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
/// redraw, which is what the tick below is: a timer on the main loop, rather than
/// a callback from the scan thread, so GTK is only ever touched from the thread
/// that owns it.
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
    bool running() const { return running_.load(std::memory_order_acquire); }

    diskscan::TreeScanner* scanner() { return scanner_.get(); }
    const diskscan::ScanResult& result() const { return result_; }
    const diskscan::NameString& root() const { return root_; }

private:
    static int tick(void* self);
    void join();

    std::unique_ptr<diskscan::TreeScanner> scanner_;
    std::thread thread_;
    diskscan::ScanResult result_;
    diskscan::NameString root_;
    std::atomic<bool> running_{false};
    /// Set by the scan thread, read by the timer, so the finish is announced from
    /// the main loop rather than from the thread that happened to end.
    std::atomic<bool> finished_{false};
    unsigned timer_ = 0;
};

}  // namespace app

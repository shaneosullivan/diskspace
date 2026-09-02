#include "scan_controller.h"

namespace app {

ScanController::ScanController() = default;

ScanController::~ScanController() {
    stop();
    join();
}

void ScanController::start(const diskscan::NameString& root) {
    stop();
    join();

    root_ = root;
    result_ = diskscan::ScanResult();

    diskscan::ScanOptions options;
    options.applyScanHistory(root);
    // No progress callback: it would arrive on the scan thread, and everything it
    // could usefully do involves drawing. The window's timer reads the same
    // figures out of the arena instead, on the thread that is allowed to draw.
    scanner_ = std::make_unique<diskscan::TreeScanner>(options);

    running_.store(true, std::memory_order_release);
    finished_.store(false, std::memory_order_release);
    announced_ = false;

    diskscan::TreeScanner* scanner = scanner_.get();
    thread_ = std::thread([this, scanner, root] {
        result_ = scanner->scan(root);
        finished_.store(true, std::memory_order_release);
    });
}

void ScanController::stop() {
    if (scanner_ && running_.load(std::memory_order_acquire)) scanner_->stop();
}

void ScanController::join() {
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

void ScanController::poll() {
    if (announced_) return;

    if (!finished_.load(std::memory_order_acquire)) {
        if (onProgress) onProgress();
        return;
    }

    // Joined here rather than in the thread, so the result is complete and the
    // scanner is quiescent before anything is told the scan is over.
    join();
    announced_ = true;
    if (onProgress) onProgress();
    if (onFinished) onFinished();
}

}  // namespace app

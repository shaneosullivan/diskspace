#include "scan_controller.h"

#include <glib.h>

namespace app {

ScanController::ScanController() = default;

ScanController::~ScanController() {
    stop();
    join();
    if (timer_) g_source_remove(timer_);
}

void ScanController::start(const diskscan::NameString& root) {
    stop();
    join();

    root_ = root;
    result_ = diskscan::ScanResult();

    diskscan::ScanOptions options;
    options.applyScanHistory(root);
    // No progress callback: it would arrive on the scan thread, and everything it
    // could usefully do involves GTK. The timer below reads the same figures from
    // the arena instead, on the thread that is allowed to draw them.
    scanner_ = std::make_unique<diskscan::TreeScanner>(options);

    running_.store(true, std::memory_order_release);
    finished_.store(false, std::memory_order_release);

    diskscan::TreeScanner* scanner = scanner_.get();
    thread_ = std::thread([this, scanner, root] {
        result_ = scanner->scan(root);
        finished_.store(true, std::memory_order_release);
    });

    if (!timer_) timer_ = g_timeout_add(100, GSourceFunc(&ScanController::tick), this);
}

void ScanController::stop() {
    if (scanner_ && running_.load(std::memory_order_acquire)) scanner_->stop();
}

void ScanController::join() {
    if (thread_.joinable()) thread_.join();
    running_.store(false, std::memory_order_release);
}

int ScanController::tick(void* data) {
    auto* self = static_cast<ScanController*>(data);
    if (self->onProgress) self->onProgress();

    if (!self->finished_.load(std::memory_order_acquire)) return G_SOURCE_CONTINUE;

    // Joined here rather than in the thread, so the result is complete and the
    // scanner is quiescent before anything is told the scan is over.
    self->join();
    self->timer_ = 0;
    if (self->onProgress) self->onProgress();
    if (self->onFinished) self->onFinished();
    return G_SOURCE_REMOVE;
}

}  // namespace app

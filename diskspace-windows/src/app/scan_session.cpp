#include "scan_session.h"


namespace app {
namespace {

/// The file list and kind statistics are cheap to redraw, so they tick at 10 Hz.
/// The treemap costs far more and paces itself off this same tick.
constexpr UINT kRefreshInterval = 100;
constexpr UINT_PTR kTimerId = 1;

}  // namespace

ScanSession::~ScanSession() { cancel(); }

double ScanSession::elapsed() const {
    if (!scanning_.load() && finalElapsed_ > 0) return finalElapsed_;
    return double(GetTickCount64() - startedAt_) / 1000.0;
}

void ScanSession::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

void ScanSession::start(const std::wstring& path, ScanBackend backend) {
    cancel();
    rootPath_ = diskscan::canonicalPath(path);
    if (rootPath_.empty()) rootPath_ = path;
    startedAt_ = GetTickCount64();
    finalElapsed_ = 0;

    volume_ = diskscan::VolumeInfo();
    diskscan::volumeInfoForPath(rootPath_, &volume_);
    // "Other space on the drive" means nothing when the drive itself is what is
    // being shown, and the block is hidden in that case.
    std::wstring mount = volume_.mountPoint;
    while (mount.size() > 1 && mount.back() == L'\\') mount.pop_back();
    wholeVolume_ = _wcsicmp(mount.c_str(), rootPath_.c_str()) == 0;

    diskscan::ScanOptions options;
    options.backend = backend;
    // The worker count is left to the engine, which derives it from the number
    // of cores the machine reports. It was briefly a setting; measuring showed
    // there is no value a user could pick that beats the measured knee.
    // Read the baseline before the scan records a new one.
    options.applyScanHistory(rootPath_);
    baseline_ = options.hasSinceLastScan ? options.cutoffSinceLastScan : 0;

    scanner_ = std::make_unique<TreeScanner>(options);
    scanning_.store(true);
    wasStopped_ = false;
    elevationWouldHelp_ = false;
    ++generation_;

    TreeScanner* scanner = scanner_.get();
    std::wstring root = rootPath_;
    worker_ = std::thread([this, scanner, root] {
        ScanResult result = scanner->scan(root);
        finish(result);
    });
    startTimer();
}

void ScanSession::refreshFolder(int32_t node) {
    if (scanning_.load() || !scanner_) return;
    joinWorker();
    startedAt_ = GetTickCount64();
    finalElapsed_ = 0;
    scanning_.store(true);
    wasStopped_ = false;
    ++generation_;

    TreeScanner* scanner = scanner_.get();
    worker_ = std::thread([this, scanner, node] {
        ScanResult result = scanner->rescan(node);
        finish(result);
    });
    startTimer();
}

void ScanSession::finish(const ScanResult& result) {
    finalElapsed_ = result.elapsed;
    wasStopped_ = result.stopped;
    backendUsed_ = result.backend;
    elevationWouldHelp_ = result.mftUnavailable;
    ++generation_;
    scanning_.store(false);
    // The window owns the timer and the redraw; the scan thread only says it is
    // finished. Posting rather than sending, so this thread never waits on the UI.
    if (window_) PostMessageW(window_, kRefreshMessage, 0, 0);
}

void ScanSession::removeNode(int32_t node) {
    if (scanning_.load() || !scanner_) return;
    scanner_->remove(node);
    ++generation_;
    if (window_) PostMessageW(window_, kRefreshMessage, 0, 0);
}

void ScanSession::stop() {
    if (!scanning_.load()) return;
    if (scanner_) scanner_->stop();
}

void ScanSession::cancel() {
    // Without this the workers would keep reading the disk for a tree nothing is
    // going to display.
    if (scanner_) scanner_->stop();
    stopTimer();
    joinWorker();
    scanner_.reset();
    scanning_.store(false);
    wasStopped_ = false;
}

void ScanSession::startTimer() {
    stopTimer();
    if (window_) timer_ = SetTimer(window_, kTimerId, kRefreshInterval, nullptr);
}

void ScanSession::stopTimer() {
    if (timer_ && window_) KillTimer(window_, kTimerId);
    timer_ = 0;
}

}  // namespace app

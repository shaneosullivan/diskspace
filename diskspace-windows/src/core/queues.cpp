#include "queues.h"

namespace diskscan {

bool DirQueue::take(DirWork* out) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (done_) return false;
        if (!stack_.empty()) {
            *out = stack_.back();
            stack_.pop_back();
            return true;
        }
        cond_.wait(lock);
    }
}

void DirQueue::push(const std::vector<DirWork>& items) {
    if (items.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ += int64_t(items.size());
        stack_.insert(stack_.end(), items.begin(), items.end());
    }
    cond_.notify_all();
}

void DirQueue::completed() {
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --pending_;
        if (pending_ == 0) {
            done_ = true;
            finished = true;
        }
    }
    if (finished) cond_.notify_all();
}

void DirQueue::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
    }
    cond_.notify_all();
}

void BatchQueue::push(ScanBatch* batch) {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (items_.size() - head_ >= capacity_ && !closed_) {
            ++blockedPushes_;
            cond_.wait(lock);
        }
        items_.push_back(batch);
        size_t depth = items_.size() - head_;
        if (depth > maxDepth_) maxDepth_ = depth;
    }
    cond_.notify_all();
}

ScanBatch* BatchQueue::take() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (head_ < items_.size()) {
            ScanBatch* b = items_[head_];
            items_[head_] = nullptr;
            ++head_;
            if (head_ > 4096) {
                items_.erase(items_.begin(), items_.begin() + ptrdiff_t(head_));
                head_ = 0;
            }
            lock.unlock();
            cond_.notify_all();
            return b;
        }
        if (closed_) return nullptr;
        cond_.wait(lock);
    }
}

void BatchQueue::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    cond_.notify_all();
}

}  // namespace diskscan

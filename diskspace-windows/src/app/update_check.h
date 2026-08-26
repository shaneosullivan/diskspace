#pragma once

#include <windows.h>

#include <atomic>
#include <string>

namespace app {

/// A release newer than the one running.
struct UpdateAvailable {
    std::wstring version;
    std::wstring downloads;
    /// When the version now running was released, as the site records it, in
    /// yyyy-mm-dd. Empty when the site has no record of this build.
    std::wstring currentReleaseDate;

    /// "Version 1.0.4 is available. Yours is four months old." — the second
    /// sentence only when the site knew when this build came out.
    std::wstring summary() const;
};

/// Asks the site whether a newer version has been released.
///
/// The platform and version go with the request so the site can record which
/// builds are in use, and it answers with the newest release for that platform
/// along with the date the running version came out.
///
/// This is the only network request the app makes, and it carries nothing about
/// what was scanned.
class UpdateCheck {
public:
    /// Posted to the window that asked, once an answer has arrived and it is
    /// worth acting on. Nothing is posted when there is no newer version, or
    /// when the site could not be reached.
    static constexpr UINT kMessage = WM_APP + 20;

    static UpdateCheck& shared();

    /// Answered once per launch: reopening About must not ask again. Safe to
    /// call repeatedly; later calls reuse the first answer.
    void check(HWND notify);

    /// What the last check found, for drawing before the next one has answered.
    /// Null until a check has succeeded and found something newer.
    ///
    /// The answer is filled in on a background thread and read on the UI thread.
    /// `finished_` is the release/acquire pair that publishes it: everything
    /// written before it was set is visible to anyone who has seen it set.
    const UpdateAvailable* result() const {
        if (!finished_.load(std::memory_order_acquire)) return nullptr;
        return found_ ? &available_ : nullptr;
    }
    bool finished() const { return finished_.load(std::memory_order_acquire); }

    /// What this build reports itself as, read from its own version resource so
    /// there is one source of truth.
    static std::wstring currentVersion();

    /// Compares dotted numeric versions a component at a time, so 1.0.10 is
    /// newer than 1.0.9 — which string comparison gets backwards.
    static bool isNewer(const std::wstring& candidate, const std::wstring& current);

private:
    UpdateCheck() = default;
    void run(HWND notify);

    bool started_ = false;
    std::atomic<bool> finished_{false};
    bool found_ = false;
    UpdateAvailable available_;
};

}  // namespace app

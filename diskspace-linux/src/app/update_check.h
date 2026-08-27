#pragma once

#include <functional>
#include <string>

namespace app {

/// A release newer than the one running.
struct UpdateAvailable {
    std::string version;
    std::string downloads;
    /// When the version now running was released, as the site records it, in
    /// yyyy-mm-dd. Empty when the site has no record of this build.
    std::string currentReleaseDate;

    /// "Version 1.0.4 is available. Yours is four months old." - the second
    /// sentence only when the site knew when this build came out.
    std::string summary() const;
};

/// Asks the site whether a newer version has been released.
///
/// The platform and version go with the request so the site can record which
/// builds are in use, and it answers with the newest release for that platform
/// along with the date the running version came out.
///
/// This is the only network request the program makes, it carries nothing about
/// what was scanned, and it is off entirely when the setting says so.
class UpdateCheck {
public:
    static UpdateCheck& shared();

    /// Answered once per launch: reopening About must not ask again. Safe to call
    /// repeatedly - later calls get the first answer, immediately if it has
    /// already arrived. The callback runs on the main loop, and only when there
    /// is something newer to report.
    void check(std::function<void(const UpdateAvailable&)> found);

    /// What the last check found, for drawing before another asks. Null until a
    /// check has succeeded and found something newer.
    const UpdateAvailable* result() const { return found_ ? &available_ : nullptr; }
    bool finished() const { return finished_; }

    /// What this build reports itself as.
    static std::string currentVersion();

private:
    UpdateCheck() = default;
    static void* worker(void* self);
    void deliver();

    bool started_ = false;
    bool finished_ = false;
    bool found_ = false;
    UpdateAvailable available_;
    std::function<void(const UpdateAvailable&)> callback_;
};

}  // namespace app

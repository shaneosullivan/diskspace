#include "update_check.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "json.h"
#include "preferences.h"
#include "product.h"

namespace app {
namespace {

constexpr const char* kHost = "diskspace.io";
constexpr const char* kPath = "/api/latest-version";
/// What this build reports itself as. The site answers for every platform it
/// knows, and treats one it does not as "nothing available" rather than an error.
constexpr const char* kRuntime = "linux";

/// Run `argv` and collect its standard output, or empty on any failure.
///
/// fork and exec rather than popen, so no shell ever sees the arguments. The URL
/// here is built from compile-time constants, but an environment variable can
/// replace it for development, and a program that hands an externally supplied
/// string to /bin/sh has made a decision it will regret eventually.
std::string runAndRead(const std::vector<std::string>& argv) {
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) return std::string();

    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return std::string();
    }

    if (child == 0) {
        ::close(pipeFds[0]);
        ::dup2(pipeFds[1], STDOUT_FILENO);
        // Whatever it has to say about being unable to connect is not this
        // program's news to deliver.
        if (const int null = ::open("/dev/null", O_WRONLY); null >= 0) {
            ::dup2(null, STDERR_FILENO);
            ::close(null);
        }
        ::close(pipeFds[1]);

        std::vector<char*> raw;
        raw.reserve(argv.size() + 1);
        for (const std::string& argument : argv) raw.push_back(const_cast<char*>(argument.c_str()));
        raw.push_back(nullptr);
        ::execvp(raw[0], raw.data());
        ::_exit(127);  // exec failed: the tool is not installed
    }

    ::close(pipeFds[1]);
    std::string out;
    char buffer[4096];
    for (;;) {
        const ssize_t got = ::read(pipeFds[0], buffer, sizeof(buffer));
        if (got <= 0) break;
        out.append(buffer, size_t(got));
        // Far past anything the endpoint sends, and it stops a confused server
        // from being able to exhaust memory here.
        if (out.size() > (1u << 20)) break;
    }
    ::close(pipeFds[0]);

    int status = 0;
    ::waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return std::string();
    return out;
}

/// Fetches the endpoint. Empty on any failure: a check that could not reach the
/// site is not something to bother anyone with.
///
/// Through curl, or wget where curl is absent, rather than by speaking HTTPS
/// here. The alternative is linking a TLS library, and for one optional GET per
/// launch that is a poor trade against a binary that asks the host for nothing.
/// Both tools follow redirects when told to, which matters more than it sounds:
/// the site answers the apex with a 308 to its www host, so every request
/// arrives as one, and a client that did not follow them would never find an
/// update - silently, which is the worst way for this to be broken.
///
/// Where neither is installed the check simply does not happen. Nothing else is
/// affected, and nobody is shown an error about a feature they did not ask for.
std::string fetch(const std::string& query) {
    // Overridable so a development build can be pointed at a local copy of the
    // site, exactly as the Windows build allows.
    const char* override = std::getenv("DISKSPACE_UPDATE_URL");
    std::string target = override ? std::string(override)
                                  : std::string("https://") + kHost + kPath;
    target += query;

    const std::string agent =
        std::string("DiskSpace/") + DISKSPACE_VERSION_STRING + " (linux)";

    // Ten seconds end to end. A check that has not answered by then has already
    // failed as far as anyone waiting for a window is concerned.
    std::string body = runAndRead({"curl", "--silent", "--show-error", "--fail", "--location",
                                   "--max-redirs", "3", "--max-time", "10", "--user-agent", agent,
                                   "--header", "Accept: application/json", target});
    if (!body.empty()) return body;

    return runAndRead({"wget", "--quiet", "--output-document=-", "--timeout=10", "--tries=1",
                       "--max-redirect=3", "--user-agent=" + agent,
                       "--header=Accept: application/json", target});
}

/// "four months old", from a number of days.
std::string ageInWords(int days) {
    if (days >= 365) {
        const int years = days / 365;
        return std::to_string(years) + (years == 1 ? " year" : " years");
    }
    if (days >= 60) {
        const int months = days / 30;
        return std::to_string(months) + " months";
    }
    if (days >= 14) {
        const int weeks = days / 7;
        return std::to_string(weeks) + " weeks";
    }
    return std::to_string(days) + (days == 1 ? " day" : " days");
}

}  // namespace

std::string UpdateAvailable::summary() const {
    std::string line = "Version " + version + " is available.";
    const int days = diskscan::daysSince(currentReleaseDate);
    if (days >= 1) line += " Yours is " + ageInWords(days) + " old.";
    return line;
}

UpdateCheck& UpdateCheck::shared() {
    static UpdateCheck instance;
    return instance;
}

UpdateCheck::~UpdateCheck() {
    // Detached rather than joined: the worker holds nothing but its own strings,
    // and waiting up to ten seconds for a network timeout at exit would be a
    // strange thing to make anyone do on the way out of a disk usage tool.
    if (thread_.joinable()) thread_.detach();
}

std::string UpdateCheck::currentVersion() { return DISKSPACE_VERSION_STRING; }

bool UpdateCheck::inFlatpak() {
    // The file the runtime places in every sandbox. FLATPAK_ID is also set, but
    // only for the process the sandbox started - the file is there for anything
    // running inside it.
    static const bool inside = ::access("/.flatpak-info", F_OK) == 0;
    return inside;
}

void UpdateCheck::check(std::function<void(const UpdateAvailable&)> found) {
    callback_ = std::move(found);

    if (finished_) {
        // Already answered this launch. Reopening About asks nobody anything.
        if (found_ && callback_) callback_(available_);
        return;
    }
    if (started_) return;
    if (inFlatpak() || !Preferences::shared().checkForUpdates()) {
        // Inside a Flatpak, Flathub does the updating and the manifest grants no
        // network access, so there is nothing to ask and nobody to ask it of.
        finished_ = true;
        return;
    }
    started_ = true;

    // On a thread of its own, because the fetch blocks. The answer is delivered
    // from the window's timer, which is the only place the callback may draw.
    thread_ = std::thread([this] { worker(); });
}

void UpdateCheck::worker() {
    const std::string query = std::string("?platform=") + kRuntime +
                              "&version=" + currentVersion();
    const std::string body = fetch(query);

    if (!body.empty()) {
        const std::shared_ptr<diskscan::JsonValue> root = diskscan::parseJson(body);
        const diskscan::JsonValue* latest = diskscan::member(root.get(), "latest");
        const std::string version = diskscan::stringMember(latest, "version");
        if (!version.empty()) {
            // The site's own answer is preferred over comparing here: it knows
            // things this does not, such as a release pulled after publication.
            const diskscan::JsonValue* flag = diskscan::member(root.get(), "updateAvailable");
            const bool newer = (flag && flag->kind == diskscan::JsonValue::Kind::Bool)
                                   ? flag->boolean
                                   : diskscan::isNewerVersion(version, currentVersion());
            if (newer) {
                available_.version = version;
                available_.downloads = diskscan::stringMember(root.get(), "downloads");
                available_.currentReleaseDate = diskscan::stringMember(root.get(), "currentDate");
                found_ = true;
            }
        }
    }
    ready_.store(true, std::memory_order_release);
}

void UpdateCheck::poll() {
    if (finished_ || !started_) return;
    if (!ready_.load(std::memory_order_acquire)) return;
    if (thread_.joinable()) thread_.join();
    deliver();
}

void UpdateCheck::deliver() {
    finished_ = true;
    if (found_ && callback_) callback_(available_);
}

}  // namespace app

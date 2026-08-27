#include "update_check.h"

#include <gio/gio.h>
#include <glib.h>

#include <cstdlib>
#include <memory>
#include <string>

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

/// Reads an HTTP/1.1 response body from a stream that has already been written
/// to, or empty on any failure.
///
/// Hand-written rather than pulled in, the same way the JSON reader is. libsoup
/// would be a reasonable dependency for a program that spoke HTTP for a living;
/// this one makes a single GET, once, and would rather not add a shared library
/// to what is otherwise a self-contained binary.
/// Where a redirect points, or empty. Header names are matched case-insensitively
/// because nothing obliges a server to spell them the way this expects.
std::string headerValue(const std::string& head, const char* name) {
    std::string lowered = head;
    for (char& c : lowered) c = char(g_ascii_tolower(c));

    std::string needle = "\r\n";
    needle += name;
    needle += ":";
    const size_t at = lowered.find(needle);
    if (at == std::string::npos) return std::string();

    const size_t start = at + needle.size();
    const size_t end = head.find("\r\n", start);
    std::string value =
        head.substr(start, end == std::string::npos ? std::string::npos : end - start);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r')) value.pop_back();
    return value;
}

std::string readResponse(GInputStream* input, std::string* redirectTo) {
    std::string raw;
    char buffer[8192];
    for (;;) {
        GError* error = nullptr;
        const gssize got = g_input_stream_read(input, buffer, sizeof(buffer), nullptr, &error);
        if (got < 0) {
            if (error) g_error_free(error);
            return std::string();
        }
        if (got == 0) break;
        raw.append(buffer, size_t(got));
        // A response this size is already far past anything the endpoint sends,
        // and stops a confused server from being able to exhaust memory here.
        if (raw.size() > (1u << 20)) break;
    }

    const size_t split = raw.find("\r\n\r\n");
    if (split == std::string::npos) return std::string();
    const std::string head = raw.substr(0, split);
    if (head.compare(0, 9, "HTTP/1.1 ") != 0 && head.compare(0, 9, "HTTP/1.0 ") != 0) {
        return std::string();
    }
    const std::string status = head.substr(9, 3);
    if (status == "301" || status == "302" || status == "303" || status == "307" ||
        status == "308") {
        // The site redirects the apex to www, so an answer is only ever reached
        // through one of these. Reported up rather than followed here, so the
        // hop count stays with the caller that owns the loop.
        if (redirectTo) *redirectTo = headerValue(head, "location");
        return std::string();
    }
    if (status != "200") return std::string();

    std::string body = raw.substr(split + 4);

    // The endpoint answers with Connection: close and no compression, so the only
    // framing that needs undoing is chunked, which some proxies add.
    if (head.find("Transfer-Encoding: chunked") != std::string::npos ||
        head.find("transfer-encoding: chunked") != std::string::npos) {
        std::string joined;
        size_t at = 0;
        while (at < body.size()) {
            const size_t eol = body.find("\r\n", at);
            if (eol == std::string::npos) break;
            const unsigned long size = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
            if (size == 0) break;
            at = eol + 2;
            if (at + size > body.size()) break;
            joined.append(body, at, size);
            at += size + 2;
        }
        return joined;
    }
    return body;
}

/// One request, with no retrying. `redirectTo` comes back set when the answer
/// was a redirect rather than a body.
std::string fetchOnce(const std::string& target, std::string* redirectTo) {
    std::string url = target;
    bool secure = true;
    int port = 443;
    if (url.compare(0, 7, "http://") == 0) {
        secure = false;
        port = 80;
        url = url.substr(7);
    } else if (url.compare(0, 8, "https://") == 0) {
        url = url.substr(8);
    }

    std::string path = "/";
    const size_t slash = url.find('/');
    if (slash != std::string::npos) {
        path = url.substr(slash);
        url = url.substr(0, slash);
    }
    const size_t colon = url.find(':');
    if (colon != std::string::npos) {
        port = std::atoi(url.substr(colon + 1).c_str());
        url = url.substr(0, colon);
    }
    const std::string host = url;
    if (host.empty()) return std::string();

    GSocketClient* client = g_socket_client_new();
    g_socket_client_set_tls(client, secure ? TRUE : FALSE);
    // Ten seconds end to end. A check that has not answered by then has already
    // failed as far as anyone waiting for a window is concerned.
    g_socket_client_set_timeout(client, 10);

    GError* error = nullptr;
    GSocketConnection* connection =
        g_socket_client_connect_to_host(client, host.c_str(), guint16(port), nullptr, &error);
    if (!connection) {
        if (error) g_error_free(error);
        g_object_unref(client);
        return std::string();
    }

    const std::string request = "GET " + path +
                                " HTTP/1.1\r\n"
                                "Host: " + host + "\r\n"
                                "User-Agent: DiskSpace/" + DISKSPACE_VERSION_STRING +
                                " (linux)\r\n"
                                "Accept: application/json\r\n"
                                "Connection: close\r\n\r\n";

    GOutputStream* output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    std::string body;
    if (g_output_stream_write_all(output, request.data(), request.size(), nullptr, nullptr,
                                  nullptr)) {
        body = readResponse(g_io_stream_get_input_stream(G_IO_STREAM(connection)), redirectTo);
    }

    g_io_stream_close(G_IO_STREAM(connection), nullptr, nullptr);
    g_object_unref(connection);
    g_object_unref(client);
    return body;
}

/// Fetches the endpoint, following redirects. Empty on any failure: a check that
/// could not reach the site is not something to bother anyone with.
///
/// Redirects are not an edge case here. The site answers the apex with a 308 to
/// its www host, so every single request arrives as one, and a client that did
/// not follow them would simply never find an update - silently, which is the
/// worst way for this to be broken.
std::string fetch(const std::string& query) {
    // Overridable so a development build can be pointed at a local copy of the
    // site, exactly as the Windows build allows.
    const char* override = std::getenv("DISKSPACE_UPDATE_URL");
    std::string target = override ? std::string(override)
                                  : std::string("https://") + kHost + kPath;
    target += query;

    // Three hops is more than the one this actually needs, and stops a
    // misconfigured server from being able to loop us forever.
    for (int hop = 0; hop < 3; ++hop) {
        std::string redirect;
        const std::string body = fetchOnce(target, &redirect);
        if (!body.empty()) return body;
        if (redirect.empty()) return std::string();
        // A relative Location is legal and would otherwise be treated as a host.
        if (redirect.compare(0, 4, "http") != 0) return std::string();
        target = redirect;
    }
    return std::string();
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

std::string UpdateCheck::currentVersion() { return DISKSPACE_VERSION_STRING; }

bool UpdateCheck::inFlatpak() {
    // The file the runtime places in every sandbox. FLATPAK_ID is also set, but
    // only for the process the sandbox started - the file is there for anything
    // running inside it.
    static const bool inside = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
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

    // On a thread of its own, because the connect and the read both block. The
    // answer is delivered back on the main loop, which is the only place the
    // callback may touch a widget.
    g_thread_unref(g_thread_new("diskspace-update", &UpdateCheck::worker, this));
}

void* UpdateCheck::worker(void* data) {
    auto* self = static_cast<UpdateCheck*>(data);

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
                self->available_.version = version;
                self->available_.downloads = diskscan::stringMember(root.get(), "downloads");
                self->available_.currentReleaseDate =
                    diskscan::stringMember(root.get(), "currentDate");
                self->found_ = true;
            }
        }
    }

    g_idle_add(
        [](gpointer data) -> gboolean {
            static_cast<UpdateCheck*>(data)->deliver();
            return G_SOURCE_REMOVE;
        },
        self);
    return nullptr;
}

void UpdateCheck::deliver() {
    finished_ = true;
    if (found_ && callback_) callback_(available_);
}

}  // namespace app

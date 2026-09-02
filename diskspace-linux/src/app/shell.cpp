#include "shell.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

namespace app {
namespace shell {
namespace {

/// Start `argv` and do not wait for it. False when it could not be started.
///
/// Double-forked, so the intermediate child is reaped immediately and the
/// grandchild is inherited by init. Without that a file manager left running for
/// an hour would sit in this process's table as a zombie, and this program has
/// no SIGCHLD handler to collect it.
bool spawn(const std::vector<std::string>& argv) {
    const pid_t first = ::fork();
    if (first < 0) return false;
    if (first == 0) {
        if (::fork() == 0) {
            if (const int null = ::open("/dev/null", O_RDWR); null >= 0) {
                ::dup2(null, STDIN_FILENO);
                ::dup2(null, STDOUT_FILENO);
                ::dup2(null, STDERR_FILENO);
                if (null > 2) ::close(null);
            }
            std::vector<char*> raw;
            raw.reserve(argv.size() + 1);
            for (const std::string& argument : argv) {
                raw.push_back(const_cast<char*>(argument.c_str()));
            }
            raw.push_back(nullptr);
            ::execvp(raw[0], raw.data());
            ::_exit(127);
        }
        ::_exit(0);
    }
    int status = 0;
    ::waitpid(first, &status, 0);
    return true;
}

std::string dataHome() {
    if (const char* xdg = ::getenv("XDG_DATA_HOME"); xdg && *xdg) return xdg;
    if (const char* home = ::getenv("HOME"); home && *home) return std::string(home) + "/.local/share";
    return std::string();
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// Local time as the trash specification wants it: yyyy-mm-ddThh:mm:ss.
std::string deletionDate() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    ::localtime_r(&now, &parts);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
    return buffer;
}

/// Percent-encode the bytes a trashinfo Path may not carry literally.
///
/// The specification says the path is encoded as a URL path. A filename is
/// arbitrary bytes, so this escapes everything outside the unreserved set rather
/// than trying to decide what is "safe" - which also means a name that is not
/// valid UTF-8 survives the round trip, and file managers put it back where it
/// came from.
std::string urlEncode(const std::string& value) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size());
    for (unsigned char c : value) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                           c == '~' || c == '/';
        if (plain) {
            out.push_back(char(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

bool makeDirectories(const std::string& path) {
    std::string built;
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            built = path.substr(0, i);
            if (::mkdir(built.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
    }
    return true;
}

/// A name inside `directory` that is not taken, starting from `wanted`.
std::string freeName(const std::string& directory, const std::string& wanted) {
    struct stat unused;
    std::string candidate = wanted;
    for (int attempt = 1; attempt < 10000; ++attempt) {
        const std::string full = directory + "/" + candidate;
        if (::lstat(full.c_str(), &unused) != 0) return candidate;
        // The convention every trash implementation uses: the name, a number,
        // and the extension kept where there is one.
        const size_t dot = wanted.find_last_of('.');
        if (dot == std::string::npos || dot == 0) {
            candidate = wanted + "." + std::to_string(attempt);
        } else {
            candidate = wanted.substr(0, dot) + "." + std::to_string(attempt) + wanted.substr(dot);
        }
    }
    return std::string();
}

/// Move into the trash directory at `trashRoot`, writing the info file first.
///
/// The info file goes first deliberately. If the program dies between the two
/// steps, an info file with no matching file is harmless and file managers ignore
/// it; a trashed file with no info file is an orphan nothing can restore.
bool trashInto(const std::string& trashRoot, const std::string& path,
               const std::string& recordedPath, std::string* error) {
    const std::string filesDirectory = trashRoot + "/files";
    const std::string infoDirectory = trashRoot + "/info";
    if (!makeDirectories(filesDirectory) || !makeDirectories(infoDirectory)) {
        *error = "Could not create the trash directory at " + trashRoot + ".";
        return false;
    }

    const std::string name = freeName(filesDirectory, baseName(path));
    if (name.empty()) {
        *error = "There is already something in the trash with that name.";
        return false;
    }

    {
        std::ofstream info(infoDirectory + "/" + name + ".trashinfo", std::ios::trunc);
        if (!info) {
            *error = "Could not write the trash record.";
            return false;
        }
        info << "[Trash Info]\n"
             << "Path=" << urlEncode(recordedPath) << "\n"
             << "DeletionDate=" << deletionDate() << "\n";
    }

    const std::string destination = filesDirectory + "/" + name;
    if (::rename(path.c_str(), destination.c_str()) != 0) {
        const int failure = errno;
        ::unlink((infoDirectory + "/" + name + ".trashinfo").c_str());
        if (failure == EXDEV) return false;  // the caller tries the volume's own trash
        *error = std::strerror(failure);
        return false;
    }
    return true;
}

/// The mount point of the filesystem `path` is on.
///
/// Walked upwards until the device number changes, which is the definition of a
/// mount point and needs no /proc/mounts parsing.
std::string topDirectory(const std::string& path) {
    struct stat self {};
    if (::lstat(path.c_str(), &self) != 0) return std::string();

    std::string current = path;
    for (;;) {
        const size_t slash = current.find_last_of('/');
        if (slash == std::string::npos) return "/";
        const std::string parent = slash == 0 ? "/" : current.substr(0, slash);
        struct stat above {};
        if (::stat(parent.c_str(), &above) != 0) return current;
        if (above.st_dev != self.st_dev) return current;
        if (parent == "/") return "/";
        current = parent;
    }
}

}  // namespace

bool openPath(const diskscan::NameString& path) {
    if (path.empty()) return false;
    return spawn({"xdg-open", path});
}

bool revealPath(const diskscan::NameString& path) {
    if (path.empty()) return false;
    // The freedesktop interface for exactly this, which Nautilus, Dolphin, Thunar
    // and Nemo all answer. Failing that, open the containing folder, which is the
    // same thing without the file selected.
    const size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string(".")
                               : slash == 0              ? std::string("/")
                                                         : path.substr(0, slash);
    if (spawn({"dbus-send", "--session", "--print-reply", "--dest=org.freedesktop.FileManager1",
               "--type=method_call", "/org/freedesktop/FileManager1",
               "org.freedesktop.FileManager1.ShowItems", "array:string:file://" + path,
               "string:"})) {
        return true;
    }
    return spawn({"xdg-open", parent});
}

bool moveToTrash(const diskscan::NameString& path, std::string* error) {
    std::string ignored;
    if (!error) error = &ignored;
    *error = std::string();
    if (path.empty() || path == "/") {
        *error = "That is not something that can be moved to the trash.";
        return false;
    }

    // The home trash first, which is where everything on the same filesystem as
    // the home directory belongs.
    const std::string home = dataHome();
    if (!home.empty()) {
        if (trashInto(home + "/Trash", path, path, error)) return true;
        if (!error->empty()) return false;
    }

    // A different filesystem. The specification puts the trash at the top of that
    // volume instead, named for the user, and records the path relative to it.
    const std::string top = topDirectory(path);
    if (!top.empty() && top != "/") {
        const std::string relative =
            path.size() > top.size() ? path.substr(top.size() + 1) : baseName(path);
        if (trashInto(top + "/.Trash-" + std::to_string(::getuid()), path, relative, error)) {
            return true;
        }
    }

    if (error->empty()) *error = "It is on a filesystem with nowhere to put a trash directory.";
    return false;
}

}  // namespace shell
}  // namespace app

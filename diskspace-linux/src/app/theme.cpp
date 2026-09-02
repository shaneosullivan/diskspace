#include "theme.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace app {
namespace {

std::string homeFile(const char* relative) {
    const char* config = ::getenv("XDG_CONFIG_HOME");
    std::string base;
    if (config && *config) {
        base = config;
    } else if (const char* home = ::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.config";
    } else {
        return std::string();
    }
    return base + "/" + relative;
}

/// Whether `path` asks for a dark theme, by either of the two ways a GTK
/// settings file can say so.
bool fileWantsDark(const std::string& path, bool* found) {
    if (path.empty()) return false;
    std::ifstream file(path);
    if (!file) return false;
    std::string line;
    bool dark = false;
    while (std::getline(file, line)) {
        // Lower-cased and stripped of spaces, so "Gtk-Theme-Name = Adwaita-dark"
        // reads the same as "gtk-theme-name=Adwaita-dark".
        std::string flat;
        for (char c : line) {
            if (c == ' ' || c == '\t' || c == '\r') continue;
            flat.push_back(char(::tolower((unsigned char)c)));
        }
        if (flat.rfind("gtk-application-prefer-dark-theme=", 0) == 0) {
            const std::string value = flat.substr(flat.find('=') + 1);
            *found = true;
            if (value == "1" || value == "true") dark = true;
        } else if (flat.rfind("gtk-theme-name=", 0) == 0) {
            const std::string value = flat.substr(flat.find('=') + 1);
            *found = true;
            if (value.find("dark") != std::string::npos) dark = true;
        }
    }
    return dark;
}

}  // namespace

Theme Theme::light() {
    Theme theme;
    theme.dark = false;
    theme.background = ui::rgb(0xf6, 0xf5, 0xf4);
    theme.surface = ui::rgb(0xff, 0xff, 0xff);
    theme.surfaceHover = ui::rgb(0xea, 0xe9, 0xe8);
    theme.surfacePressed = ui::rgb(0xdc, 0xdb, 0xda);
    theme.border = ui::rgb(0xd0, 0xcf, 0xce);
    theme.text = ui::rgb(0x20, 0x20, 0x22);
    theme.dimText = ui::rgb(0x6a, 0x6a, 0x70);
    theme.selection = ui::rgb(0x35, 0x84, 0xe4);
    theme.selectionText = ui::rgb(0xff, 0xff, 0xff);
    theme.accent = ui::rgb(0x35, 0x84, 0xe4);
    theme.well = ui::rgb(0xff, 0xff, 0xff);
    return theme;
}

Theme Theme::darkTheme() {
    Theme theme;
    theme.dark = true;
    theme.background = ui::rgb(0x24, 0x24, 0x28);
    theme.surface = ui::rgb(0x2e, 0x2e, 0x33);
    theme.surfaceHover = ui::rgb(0x3a, 0x3a, 0x40);
    theme.surfacePressed = ui::rgb(0x45, 0x45, 0x4c);
    theme.border = ui::rgb(0x44, 0x44, 0x4a);
    theme.text = ui::rgb(0xea, 0xea, 0xee);
    theme.dimText = ui::rgb(0x9a, 0x9a, 0xa4);
    theme.selection = ui::rgb(0x35, 0x84, 0xe4);
    theme.selectionText = ui::rgb(0xff, 0xff, 0xff);
    theme.accent = ui::rgb(0x62, 0xa0, 0xea);
    theme.well = ui::rgb(0x1c, 0x1c, 0x20);
    return theme;
}

Theme Theme::detect() {
    // An explicit answer wins, which is also how this gets tested: a screenshot
    // of both themes needs no desktop to be reconfigured between them.
    if (const char* forced = ::getenv("DISKSPACE_THEME"); forced && *forced) {
        if (std::string(forced) == "dark") return darkTheme();
        if (std::string(forced) == "light") return light();
    }
    // GTK's own convention, which every desktop that has a dark mode honours.
    if (const char* gtkTheme = ::getenv("GTK_THEME"); gtkTheme && *gtkTheme) {
        if (std::string(gtkTheme).find("dark") != std::string::npos) return darkTheme();
    }

    // The settings file the desktop writes when the preference is changed. GTK 4
    // first, since a machine with both has usually had the GTK 3 one left behind
    // by something older.
    bool found = false;
    if (fileWantsDark(homeFile("gtk-4.0/settings.ini"), &found)) return darkTheme();
    if (found) return light();
    if (fileWantsDark(homeFile("gtk-3.0/settings.ini"), &found)) return darkTheme();

    return light();
}

}  // namespace app

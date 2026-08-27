#include "preferences_window.h"

#include <functional>

#include "preferences.h"
#include "product.h"

namespace app {
namespace {

using Getter = bool (Preferences::*)() const;
using Setter = void (Preferences::*)(bool);

struct Row {
    const char* title;
    const char* subtitle;
    Getter get;
    Setter set;
};

/// The switch's own state is the source of truth for the widget; Preferences is
/// the source of truth for the program. Bound one way each: toggling writes
/// through, and nothing writes back, because this window is the only thing that
/// changes these while it is open.
void bind(GtkWidget* toggle, Getter get, Setter set) {
    gtk_switch_set_active(GTK_SWITCH(toggle), (Preferences::shared().*get)());
    auto* holder = new Setter(set);
    g_object_set_data_full(G_OBJECT(toggle), "setter", holder,
                           [](gpointer data) { delete static_cast<Setter*>(data); });
    g_signal_connect(toggle, "state-set",
                     G_CALLBACK(+[](GtkSwitch* self, gboolean state, gpointer) -> gboolean {
                         auto* setter = static_cast<Setter*>(
                             g_object_get_data(G_OBJECT(self), "setter"));
                         if (setter) (Preferences::shared().**setter)(state != FALSE);
                         return FALSE;  // let the switch take the new state
                     }),
                     nullptr);
}

GtkWidget* section(const char* title) {
    GtkWidget* label = gtk_label_new(nullptr);
    char* markup = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_widget_set_margin_top(label, 10);
    return label;
}

GtkWidget* switchRow(const Row& row) {
    GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_hexpand(text, TRUE);

    GtkWidget* title = gtk_label_new(row.title);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(text), title);

    if (row.subtitle) {
        GtkWidget* subtitle = gtk_label_new(row.subtitle);
        gtk_widget_set_halign(subtitle, GTK_ALIGN_START);
        gtk_label_set_wrap(GTK_LABEL(subtitle), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 52);
        gtk_widget_add_css_class(subtitle, "dim-label");
        gtk_box_append(GTK_BOX(text), subtitle);
    }

    GtkWidget* toggle = gtk_switch_new();
    gtk_widget_set_valign(toggle, GTK_ALIGN_CENTER);
    bind(toggle, row.get, row.set);

    gtk_box_append(GTK_BOX(line), text);
    gtk_box_append(GTK_BOX(line), toggle);
    return line;
}

}  // namespace

void presentPreferences(GtkWindow* parent) {
    GtkWidget* window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), "Settings");
    gtk_window_set_transient_for(GTK_WINDOW(window), parent);
    gtk_window_set_modal(GTK_WINDOW(window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(window), 480, -1);

    GtkWidget* column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(column, 18);
    gtk_widget_set_margin_bottom(column, 18);
    gtk_widget_set_margin_start(column, 18);
    gtk_widget_set_margin_end(column, 18);

    static const Row kMeasurement[] = {
        {"Show space occupied on disk",
         "Rather than the length of a file's contents. The two differ sharply for "
         "sparse and compressed files.",
         &Preferences::showPhysicalFileSize, &Preferences::setShowPhysicalFileSize},
    };
    static const Row kAppearance[] = {
        {"Consistent colours for file kinds",
         "A file type looks the same in every scan. Turned off, the largest kinds "
         "in this scan get the most distinct hues.",
         &Preferences::consistentKindColours, &Preferences::setConsistentKindColours},
        {"List above the map", "Rather than beside it.", &Preferences::splitHorizontally,
         &Preferences::setSplitHorizontally},
        {"Show the file kinds table", nullptr, &Preferences::showFileKinds,
         &Preferences::setShowFileKinds},
    };
    static const Row kMap[] = {
        {"Show free space",
         "Draws the room left on the volume as a block, to size what was scanned "
         "against it.",
         &Preferences::showFreeSpace, &Preferences::setShowFreeSpace},
        {"Show other space in use",
         "Everything else on the volume, outside what was scanned. Hidden when a "
         "whole volume is on screen, where it would be nothing.",
         &Preferences::showOtherSpace, &Preferences::setShowOtherSpace},
    };
    static const Row kPrivacy[] = {
        {"Check for updates at launch",
         "Asks the site whether a newer version exists, sending only the platform "
         "and the version running. It is the only network request this program "
         "makes, and nothing about what you scanned is ever sent.",
         &Preferences::checkForUpdates, &Preferences::setCheckForUpdates},
    };

    auto addAll = [&](const char* title, const Row* rows, size_t count) {
        gtk_box_append(GTK_BOX(column), section(title));
        for (size_t i = 0; i < count; ++i) gtk_box_append(GTK_BOX(column), switchRow(rows[i]));
    };
    addAll("Measurement", kMeasurement, G_N_ELEMENTS(kMeasurement));
    addAll("Appearance", kAppearance, G_N_ELEMENTS(kAppearance));
    addAll("The map", kMap, G_N_ELEMENTS(kMap));
    addAll("Privacy", kPrivacy, G_N_ELEMENTS(kPrivacy));

    GtkWidget* footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(footer, 14);
    GtkWidget* version = gtk_label_new(DISKSPACE_APP_NAME " " DISKSPACE_VERSION_STRING);
    gtk_widget_add_css_class(version, "dim-label");
    gtk_widget_set_hexpand(version, TRUE);
    gtk_widget_set_halign(version, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(footer), version);

    GtkWidget* reset = gtk_button_new_with_label("Reset to Defaults");
    g_signal_connect(reset, "clicked", G_CALLBACK(+[](GtkButton* self, gpointer) {
                         Preferences::shared().resetAll();
                         // Every switch is stale now, and rebuilding the window is
                         // less code than walking it to re-read each one.
                         GtkRoot* root = gtk_widget_get_root(GTK_WIDGET(self));
                         GtkWindow* parent =
                             gtk_window_get_transient_for(GTK_WINDOW(root));
                         gtk_window_destroy(GTK_WINDOW(root));
                         presentPreferences(parent);
                     }),
                     nullptr);
    gtk_box_append(GTK_BOX(footer), reset);
    gtk_box_append(GTK_BOX(column), footer);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), column);
    gtk_window_set_child(GTK_WINDOW(window), scroller);
    gtk_window_present(GTK_WINDOW(window));
}

}  // namespace app

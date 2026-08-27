#include "theme.h"

namespace app {

GdkRGBA foreground(GtkWidget* widget) {
    GdkRGBA colour;
    gtk_widget_get_color(widget, &colour);
    // A widget not yet realised answers with transparent black, which would draw
    // nothing at all. Dark grey is the better guess for a first frame.
    if (colour.alpha <= 0.01f) return GdkRGBA{0.15f, 0.15f, 0.15f, 1.0f};
    return colour;
}

bool isDark(GtkWidget* widget) {
    const GdkRGBA fg = foreground(widget);
    // Rec. 709 luma. Light text means the surface behind it is dark.
    const double luma = 0.2126 * fg.red + 0.7152 * fg.green + 0.0722 * fg.blue;
    return luma > 0.5;
}

GdkRGBA selection(GtkWidget* widget) {
    return isDark(widget) ? GdkRGBA{0.22f, 0.45f, 0.78f, 1.0f}
                          : GdkRGBA{0.20f, 0.44f, 0.86f, 1.0f};
}

}  // namespace app

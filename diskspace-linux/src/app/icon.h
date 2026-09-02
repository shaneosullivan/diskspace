#pragma once

#include <cstdint>
#include <vector>

#include "x11.h"

namespace app {

/// The application's icon, compiled into the binary.
///
/// Loaded from an embedded PNG rather than from a file beside the executable,
/// because there is no file beside the executable: an AppImage is mounted at a
/// different path every launch and the loose binary might be anywhere. Decoded
/// with libpng, which is already in the process - freetype links it for colour
/// fonts - so this costs no new dependency at runtime.
class Icon {
public:
    /// Decode the embedded image. Safe to call repeatedly; only the first does
    /// any work. False when the image could not be read, in which case draw()
    /// does nothing and the interface simply has no icon in it.
    bool load();

    /// Draw at `size` square, composited over `background`.
    ///
    /// The icon has an alpha channel and the canvas blit does not, so the two
    /// are combined here against a colour the caller knows. That is exact rather
    /// than approximate, because what sits behind it is a flat fill.
    void draw(ui::Canvas& canvas, int x, int y, int size, ui::Color background);

private:
    /// The icon resampled to one size, kept because the welcome screen redraws
    /// on every pointer move and resampling on each would be silly.
    struct Scaled {
        int size = 0;
        ui::Color background = 0;
        std::vector<uint32_t> pixels;
    };

    const Scaled* scaledTo(int size, ui::Color background);

    bool loaded_ = false;
    bool failed_ = false;
    int width_ = 0;
    int height_ = 0;
    /// Straight (not premultiplied) RGBA, one pixel per element as 0xAARRGGBB.
    std::vector<uint32_t> source_;
    std::vector<Scaled> cache_;
};

}  // namespace app

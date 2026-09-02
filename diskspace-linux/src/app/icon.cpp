#include "icon.h"

#include <png.h>

#include <algorithm>
#include <cstring>

#include "app_icon_png.h"

namespace app {
namespace {

/// Where libpng has read up to in the embedded array.
struct Cursor {
    const uint8_t* data;
    size_t size;
    size_t at;
};

void readFromMemory(png_structp png, png_bytep out, png_size_t wanted) {
    auto* cursor = static_cast<Cursor*>(png_get_io_ptr(png));
    if (!cursor || cursor->at + wanted > cursor->size) {
        png_error(png, "read past the end of the embedded icon");
        return;
    }
    std::memcpy(out, cursor->data + cursor->at, wanted);
    cursor->at += wanted;
}

}  // namespace

bool Icon::load() {
    if (loaded_) return true;
    if (failed_) return false;
    failed_ = true;  // until proven otherwise, so a failure is not retried

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return false;
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        return false;
    }
    // libpng reports errors by longjmp, so this is the failure path for
    // everything below rather than a return value to check.
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    Cursor cursor{kAppIconPng, kAppIconPngSize, 0};
    png_set_read_fn(png, &cursor, &readFromMemory);
    png_read_info(png, info);

    width_ = int(png_get_image_width(png, info));
    height_ = int(png_get_image_height(png, info));
    const png_byte colour = png_get_color_type(png, info);
    const png_byte depth = png_get_bit_depth(png, info);

    // Normalised to eight-bit RGBA whatever the file happens to be, so the rest
    // of this has one case to handle.
    if (depth == 16) png_set_strip_16(png);
    if (colour == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (colour == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (colour == PNG_COLOR_TYPE_GRAY || colour == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (colour == PNG_COLOR_TYPE_RGB || colour == PNG_COLOR_TYPE_GRAY ||
        colour == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    png_read_update_info(png, info);

    if (width_ <= 0 || height_ <= 0 || width_ > 4096 || height_ > 4096) {
        png_destroy_read_struct(&png, &info, nullptr);
        return false;
    }

    std::vector<uint8_t> rows(size_t(width_) * size_t(height_) * 4);
    // Braced, because `rowPointers(size_t(height_))` is read as a function
    // declaration rather than a vector - the most vexing parse, which the
    // compiler reports as an assignment to a read-only location several lines
    // further down.
    std::vector<png_bytep> rowPointers{};
    rowPointers.resize(size_t(height_));
    for (int y = 0; y < height_; ++y) rowPointers[size_t(y)] = rows.data() + size_t(y) * width_ * 4;
    png_read_image(png, rowPointers.data());
    png_destroy_read_struct(&png, &info, nullptr);

    source_.resize(size_t(width_) * size_t(height_));
    for (size_t i = 0; i < source_.size(); ++i) {
        const uint8_t* p = rows.data() + i * 4;
        source_[i] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) |
                     uint32_t(p[2]);
    }

    loaded_ = true;
    failed_ = false;
    return true;
}

const Icon::Scaled* Icon::scaledTo(int size, ui::Color background) {
    for (const Scaled& cached : cache_) {
        if (cached.size == size && cached.background == background) return &cached;
    }

    Scaled scaled;
    scaled.size = size;
    scaled.background = background;
    scaled.pixels.assign(size_t(size) * size_t(size), background & 0x00FFFFFFu);

    const double bgR = double((background >> 16) & 0xFF);
    const double bgG = double((background >> 8) & 0xFF);
    const double bgB = double(background & 0xFF);

    // A box filter: every destination pixel averages the source pixels it covers.
    // The icon is far larger than it is drawn, so this is a downscale and a box
    // is the right answer - it uses every source pixel exactly once, which is
    // what stops a detailed icon from crawling with aliasing at small sizes.
    for (int y = 0; y < size; ++y) {
        const int sourceTop = int(int64_t(y) * height_ / size);
        const int sourceBottom = std::max(sourceTop + 1, int(int64_t(y + 1) * height_ / size));
        for (int x = 0; x < size; ++x) {
            const int sourceLeft = int(int64_t(x) * width_ / size);
            const int sourceRight = std::max(sourceLeft + 1, int(int64_t(x + 1) * width_ / size));

            // Averaged with alpha as a weight, so a transparent pixel does not
            // drag its colour - usually black - into the visible edge.
            double weight = 0, r = 0, g = 0, b = 0, a = 0;
            for (int sy = sourceTop; sy < sourceBottom; ++sy) {
                for (int sx = sourceLeft; sx < sourceRight; ++sx) {
                    const uint32_t pixel = source_[size_t(sy) * size_t(width_) + size_t(sx)];
                    const double alpha = double((pixel >> 24) & 0xFF) / 255.0;
                    r += double((pixel >> 16) & 0xFF) * alpha;
                    g += double((pixel >> 8) & 0xFF) * alpha;
                    b += double(pixel & 0xFF) * alpha;
                    a += alpha;
                    weight += 1;
                }
            }
            if (weight <= 0) continue;

            const double coverage = a / weight;
            // Back out of the alpha weighting, then composite over the known
            // background: the canvas blit carries no alpha of its own.
            const double outR = a > 0 ? r / a : 0;
            const double outG = a > 0 ? g / a : 0;
            const double outB = a > 0 ? b / a : 0;
            const auto mix = [coverage](double over, double under) {
                return uint32_t(std::clamp(over * coverage + under * (1.0 - coverage), 0.0, 255.0));
            };
            scaled.pixels[size_t(y) * size_t(size) + size_t(x)] =
                (mix(outR, bgR) << 16) | (mix(outG, bgG) << 8) | mix(outB, bgB);
        }
    }

    // Two entries is all this ever holds - one theme, one size - but a bound is
    // cheap and stops a resize storm from growing it without limit.
    if (cache_.size() >= 4) cache_.erase(cache_.begin());
    cache_.push_back(std::move(scaled));
    return &cache_.back();
}

void Icon::draw(ui::Canvas& canvas, int x, int y, int size, ui::Color background) {
    if (!load() || size <= 0) return;
    const Scaled* scaled = scaledTo(size, background);
    if (!scaled) return;
    canvas.image(x, y, scaled->pixels.data(), size, size);
}

}  // namespace app

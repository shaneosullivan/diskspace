#include "x11.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xrender.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace ui {
namespace {

/// Xft wants sixteen bits per channel, premultiplied.
XftColor toXft(Color color) {
    const uint8_t a = uint8_t(color >> 24);
    const uint8_t r = uint8_t(color >> 16);
    const uint8_t g = uint8_t(color >> 8);
    const uint8_t b = uint8_t(color);
    XftColor out{};
    out.color.alpha = uint16_t(a * 257);
    out.color.red = uint16_t(r * a * 257 / 255);
    out.color.green = uint16_t(g * a * 257 / 255);
    out.color.blue = uint16_t(b * a * 257 / 255);
    out.pixel = 0;
    return out;
}

/// The byte before `index` that starts a UTF-8 sequence, so a string is never
/// cut through the middle of a character. A truncated sequence would not render
/// at all, and on a list of filenames that shows up as a row that is suddenly
/// blank rather than merely shortened.
size_t previousBoundary(const char* text, size_t index) {
    while (index > 0 && (uint8_t(text[index]) & 0xC0) == 0x80) --index;
    return index;
}

constexpr const char* kEllipsis = "\xE2\x80\xA6";  // U+2026

/// One code point from UTF-8, advancing `index`.
///
/// Malformed input is not an error here. A Linux filename is a byte string with
/// no encoding guarantee, so this has to produce something for every byte rather
/// than refuse: a bad byte becomes U+FFFD and the walk carries on, which keeps a
/// strangely named file visible in the list instead of dropping it.
uint32_t nextRune(const char* text, size_t length, size_t* index) {
    const uint8_t first = uint8_t(text[*index]);
    size_t extra = 0;
    uint32_t rune = 0;
    if (first < 0x80) {
        ++*index;
        return first;
    } else if ((first & 0xE0) == 0xC0) {
        extra = 1;
        rune = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        extra = 2;
        rune = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        extra = 3;
        rune = first & 0x07u;
    } else {
        ++*index;
        return 0xFFFD;
    }
    if (*index + extra >= length) {
        ++*index;
        return 0xFFFD;
    }
    for (size_t i = 1; i <= extra; ++i) {
        const uint8_t byte = uint8_t(text[*index + i]);
        if ((byte & 0xC0) != 0x80) {
            ++*index;
            return 0xFFFD;
        }
        rune = (rune << 6) | (byte & 0x3Fu);
    }
    *index += extra + 1;
    return rune;
}

}  // namespace

Rect Rect::intersect(const Rect& other) const {
    const int left = std::max(x, other.x);
    const int top = std::max(y, other.y);
    const int right = std::min(this->right(), other.right());
    const int bottom = std::min(this->bottom(), other.bottom());
    if (right <= left || bottom <= top) return Rect{};
    return Rect{left, top, right - left, bottom - top};
}

Color blend(Color under, Color over) {
    const uint32_t a = over >> 24;
    if (a == 255) return over | 0xFF000000u;
    if (a == 0) return under;
    auto mix = [&](int shift) {
        const uint32_t u = (under >> shift) & 0xFF;
        const uint32_t o = (over >> shift) & 0xFF;
        return ((o * a + u * (255 - a)) / 255) << shift;
    };
    return 0xFF000000u | mix(16) | mix(8) | mix(0);
}

// =============================================================================
// Display
// =============================================================================

Display* Display::open() {
    auto* self = new Display();
    if (!self->init()) {
        delete self;
        return nullptr;
    }
    return self;
}

bool Display::init() {
    display_ = XOpenDisplay(nullptr);
    if (!display_) return false;
    screen_ = DefaultScreen(display_);
    visual_ = DefaultVisual(display_, screen_);
    colormap_ = DefaultColormap(display_, screen_);
    depth_ = DefaultDepth(display_, screen_);

    // The pixel path assumes a little-endian 24- or 32-bit true colour visual,
    // which is every x86 desktop made this century. Somewhere else it would need
    // a conversion in image(), so say so rather than drawing nonsense.
    if (depth_ < 24) {
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }

    // Xft.dpi is what desktops set and toolkits read. Failing that, the screen's
    // millimetre size, which is often a lie but a better one than assuming.
    double dpi = 0;
    if (char* resources = XResourceManagerString(display_)) {
        XrmInitialize();
        if (XrmDatabase database = XrmGetStringDatabase(resources)) {
            char* type = nullptr;
            XrmValue value{};
            if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr) {
                dpi = atof(value.addr);
            }
            XrmDestroyDatabase(database);
        }
    }
    if (dpi <= 0) {
        const int pixels = DisplayHeight(display_, screen_);
        const int millimetres = DisplayHeightMM(display_, screen_);
        if (millimetres > 0) dpi = double(pixels) * 25.4 / double(millimetres);
    }
    if (dpi <= 0) dpi = 96.0;
    // Clamped: a monitor reporting its size wrongly is common, and a scale of
    // seven makes the application unusable rather than merely ugly.
    scale_ = std::clamp(dpi / 96.0, 0.75, 4.0);
    return true;
}

Display::~Display() {
    for (CachedFont& cached : fonts_) {
        for (XftFont* fallback : cached.fallbacks) XftFontClose(display_, fallback);
        if (cached.font.handle) XftFontClose(display_, cached.font.handle);
    }
    if (display_) XCloseDisplay(display_);
}

Font Display::font(double points, Weight weight) {
    for (const CachedFont& cached : fonts_) {
        if (cached.points == points && cached.weight == weight) return cached.font;
    }
    // Points to pixels through the scale already resolved, rather than letting
    // Xft consult the resource again: one place decides how big things are.
    const double pixels = points * 96.0 / 72.0 * scale_;
    XftFont* handle = XftFontOpen(
        display_, screen_, FC_FAMILY, FcTypeString, "sans-serif", FC_PIXEL_SIZE, FcTypeDouble,
        pixels, FC_WEIGHT, FcTypeInteger, weight == Weight::Bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR,
        nullptr);
    if (!handle) {
        handle = XftFontOpen(display_, screen_, FC_FAMILY, FcTypeString, "sans", FC_PIXEL_SIZE,
                             FcTypeDouble, pixels, nullptr);
    }
    Font font;
    font.handle = handle;
    font.id = int(fonts_.size());
    if (handle) {
        font.ascent = handle->ascent;
        font.descent = handle->descent;
        font.lineHeight = handle->ascent + handle->descent;
    }
    fonts_.push_back({points, weight, font, {}, {}});
    return font;
}

XftFont* Display::faceFor(int fontId, uint32_t rune) {
    if (fontId < 0 || size_t(fontId) >= fonts_.size()) return nullptr;
    CachedFont& cached = fonts_[size_t(fontId)];
    XftFont* base = cached.font.handle;
    if (!base) return nullptr;
    if (XftCharExists(display_, base, FcChar32(rune))) return base;

    for (XftFont* candidate : cached.fallbacks) {
        if (XftCharExists(display_, candidate, FcChar32(rune))) return candidate;
    }
    for (uint32_t missing : cached.hopeless) {
        if (missing == rune) return base;
    }

    // Ask fontconfig which installed face has this character, at the size and
    // weight the base font was asked for, so the fallback does not turn up in a
    // different size to the text around it.
    //
    // Built fresh rather than by copying the base font's pattern and dropping
    // the family from it. That copy is the *resolved* pattern: it carries
    // FC_FILE, FC_INDEX and the rest of the identity of the face that was
    // matched, and those pin the search straight back to it. Measured here -
    // duplicating the pattern and deleting the family returned DejaVu Sans
    // again, which is the font that had just been found not to have the
    // character. A fresh pattern returns Droid Sans Fallback, which does.
    FcCharSet* wanted = FcCharSetCreate();
    FcCharSetAddChar(wanted, FcChar32(rune));
    FcPattern* pattern = FcPatternCreate();
    FcPatternAddCharSet(pattern, FC_CHARSET, wanted);
    FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, cached.points * 96.0 / 72.0 * scale_);
    FcPatternAddInteger(pattern, FC_WEIGHT,
                        cached.weight == Weight::Bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);

    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, pattern, &result);
    FcPatternDestroy(pattern);
    FcCharSetDestroy(wanted);

    if (match) {
        // XftFontOpenPattern takes the pattern when it succeeds and leaves it
        // when it does not, so only the failing path may destroy it.
        if (XftFont* opened = XftFontOpenPattern(display_, match)) {
            if (XftCharExists(display_, opened, FcChar32(rune))) {
                cached.fallbacks.push_back(opened);
                return opened;
            }
            XftFontClose(display_, opened);
        } else {
            FcPatternDestroy(match);
        }
    }

    // Nothing installed can draw it. Remember that, so the search is not run
    // again for every occurrence, and let the base font show its missing glyph.
    cached.hopeless.push_back(rune);
    return base;
}

void Display::splitRuns(const char* utf8, size_t length, const Font& font,
                        std::vector<TextRun>* out) {
    out->clear();
    if (!font.handle || length == 0) return;
    size_t index = 0;
    while (index < length) {
        const size_t start = index;
        const uint32_t rune = nextRune(utf8, length, &index);
        XftFont* face = faceFor(font.id, rune);
        if (!face) face = font.handle;
        if (!out->empty() && out->back().font == face) {
            out->back().length += index - start;
        } else {
            out->push_back(TextRun{start, index - start, face});
        }
    }
}

Atom Display::atom(const char* name) { return XInternAtom(display_, name, False); }

// =============================================================================
// Canvas
// =============================================================================

Canvas::Canvas(Display* display, Drawable drawable, XftDraw* draw, GC gc, int width, int height)
    : display_(display), drawable_(drawable), draw_(draw), gc_(gc), width_(width), height_(height) {}

void Canvas::applyClip() {
    const Rect area = clip();
    XRectangle rectangle{short(area.x), short(area.y), (unsigned short)(std::max(0, area.width)),
                         (unsigned short)(std::max(0, area.height))};
    XftDrawSetClipRectangles(draw_, 0, 0, &rectangle, 1);
    XSetClipRectangles(display_->xdisplay(), gc_, 0, 0, &rectangle, 1, Unsorted);
}

void Canvas::pushClip(const Rect& rect) {
    clips_.push_back(clips_.empty() ? rect.intersect(Rect{0, 0, width_, height_})
                                    : rect.intersect(clips_.back()));
    applyClip();
}

void Canvas::popClip() {
    if (!clips_.empty()) clips_.pop_back();
    applyClip();
}

void Canvas::fill(const Rect& rect, Color color) {
    if (rect.empty()) return;

    // XRenderFillRectangle with PictOpOver rather than XftDrawRect.
    //
    // XftDrawRect composites with PictOpSrc, which replaces the destination
    // instead of blending into it. Every translucent fill therefore landed as
    // its own premultiplied colour on black: a hover highlight at ten per cent
    // white came out as near-black, and a button drawn with a transparent
    // second pass came out solid black. Nothing about that is Xft's fault - it
    // is a rectangle fill, not a compositing operation - but it means alpha has
    // to go through Render directly.
    XftColor xc = toXft(color);
    if (Picture picture = XftDrawPicture(draw_)) {
        XRenderFillRectangle(display_->xdisplay(), PictOpOver, picture, &xc.color, rect.x, rect.y,
                             (unsigned)rect.width, (unsigned)rect.height);
        return;
    }
    // No Render on this display, so there is no alpha to be had and the opaque
    // path is the only one there is.
    XftDrawRect(draw_, &xc, rect.x, rect.y, (unsigned)rect.width, (unsigned)rect.height);
}

void Canvas::frame(const Rect& rect, Color color) {
    if (rect.empty()) return;
    fill(Rect{rect.x, rect.y, rect.width, 1}, color);
    fill(Rect{rect.x, rect.bottom() - 1, rect.width, 1}, color);
    fill(Rect{rect.x, rect.y, 1, rect.height}, color);
    fill(Rect{rect.right() - 1, rect.y, 1, rect.height}, color);
}

void Canvas::line(int x1, int y1, int x2, int y2, Color color) {
    // Horizontal and vertical only, which is all this draws, and as rectangles
    // so that they share the alpha handling with everything else.
    if (y1 == y2) {
        fill(Rect{std::min(x1, x2), y1, std::abs(x2 - x1) + 1, 1}, color);
    } else if (x1 == x2) {
        fill(Rect{x1, std::min(y1, y2), 1, std::abs(y2 - y1) + 1}, color);
    }
}

void Canvas::fillRounded(const Rect& rect, int radius, Color color) {
    if (rect.empty()) return;
    radius = std::min({radius, rect.width / 2, rect.height / 2});
    if (radius <= 0) {
        fill(rect, color);
        return;
    }
    // The straight parts as rectangles, and only the four corners drawn a row at
    // a time. Each row of a corner is a rectangle whose length comes from the
    // circle, which gives a clean edge without compositing a mask: at these radii
    // the difference from a properly anti-aliased curve is not visible, and this
    // needs no XRender picture, no temporary pixmap and no cleanup.
    fill(Rect{rect.x + radius, rect.y, rect.width - 2 * radius, rect.height}, color);
    fill(Rect{rect.x, rect.y + radius, radius, rect.height - 2 * radius}, color);
    fill(Rect{rect.right() - radius, rect.y + radius, radius, rect.height - 2 * radius}, color);

    for (int row = 0; row < radius; ++row) {
        const double dy = radius - row - 0.5;
        const double dx = std::sqrt(std::max(0.0, double(radius) * radius - dy * dy));
        const int inset = radius - int(dx + 0.5);
        const int length = radius - inset;
        if (length <= 0) continue;
        fill(Rect{rect.x + inset, rect.y + row, length, 1}, color);
        fill(Rect{rect.right() - radius, rect.y + row, length, 1}, color);
        fill(Rect{rect.x + inset, rect.bottom() - row - 1, length, 1}, color);
        fill(Rect{rect.right() - radius, rect.bottom() - row - 1, length, 1}, color);
    }
}

int Canvas::textWidth(const char* utf8, size_t length, const Font& font) const {
    if (!font.handle || length == 0) return 0;
    std::vector<TextRun> runs;
    display_->splitRuns(utf8, length, font, &runs);
    int total = 0;
    for (const TextRun& run : runs) {
        XGlyphInfo info{};
        XftTextExtentsUtf8(display_->xdisplay(), run.font, (const FcChar8*)(utf8 + run.offset),
                           int(run.length), &info);
        total += info.xOff;
    }
    return total;
}

int Canvas::text(int x, int baseline, const char* utf8, size_t length, const Font& font,
                 Color color) {
    if (!font.handle || length == 0) return 0;
    std::vector<TextRun> runs;
    display_->splitRuns(utf8, length, font, &runs);
    XftColor xc = toXft(color);
    int advance = 0;
    for (const TextRun& run : runs) {
        XftDrawStringUtf8(draw_, &xc, run.font, x + advance, baseline,
                          (const FcChar8*)(utf8 + run.offset), int(run.length));
        XGlyphInfo info{};
        XftTextExtentsUtf8(display_->xdisplay(), run.font, (const FcChar8*)(utf8 + run.offset),
                           int(run.length), &info);
        advance += info.xOff;
    }
    return advance;
}

void Canvas::textIn(const Rect& box, const char* utf8, size_t length, const Font& font, Color color,
                    Align align) {
    if (!font.handle || box.empty() || length == 0) return;

    std::string shortened;
    const char* draw = utf8;
    size_t drawLength = length;
    int width = textWidth(utf8, length, font);

    if (width > box.width) {
        const int ellipsis = textWidth(kEllipsis, 3, font);
        const int room = box.width - ellipsis;
        if (room <= 0) return;
        // Longest prefix that still fits. Bisection rather than a scan: a path
        // can be thousands of characters and this runs for every visible row on
        // every repaint.
        size_t low = 0;
        size_t high = length;
        while (low < high) {
            size_t middle = previousBoundary(utf8, low + (high - low + 1) / 2);
            if (middle <= low) break;
            if (textWidth(utf8, middle, font) <= room) {
                low = middle;
            } else {
                high = middle - 1;
                high = previousBoundary(utf8, high);
            }
        }
        shortened.assign(utf8, low);
        shortened += kEllipsis;
        draw = shortened.data();
        drawLength = shortened.size();
        width = textWidth(draw, drawLength, font);
    }

    int x = box.x;
    if (align == Align::Center) x = box.x + (box.width - width) / 2;
    if (align == Align::Right) x = box.right() - width;
    const int baseline = box.y + (box.height - font.lineHeight) / 2 + font.ascent;
    text(x, baseline, draw, drawLength, font, color);
}

std::vector<std::string> Canvas::wrap(const std::string& text, const Font& font, int width) const {
    std::vector<std::string> lines;
    if (!font.handle || text.empty() || width <= 0) return lines;

    std::string line;
    size_t at = 0;
    while (at <= text.size()) {
        // A newline in the source is honoured, so a paragraph break stays one.
        const size_t breakAt = text.find_first_of(" \n", at);
        const std::string word = text.substr(at, breakAt == std::string::npos ? breakAt
                                                                              : breakAt - at);
        const std::string candidate = line.empty() ? word : line + " " + word;

        if (!line.empty() && textWidth(candidate, font) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }

        if (breakAt == std::string::npos) break;
        if (text[breakAt] == '\n') {
            lines.push_back(line);
            line.clear();
        }
        at = breakAt + 1;
    }
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

int Canvas::textBlock(const Rect& box, const std::string& text, const Font& font, Color color,
                      int lineSpacing, Align align) {
    if (!font.handle) return 0;
    const std::vector<std::string> lines = wrap(text, font, box.width);
    const int step = font.lineHeight + lineSpacing;
    int y = box.y;
    for (const std::string& line : lines) {
        if (!line.empty()) {
            textIn(Rect{box.x, y, box.width, font.lineHeight}, line, font, color, align);
        }
        y += step;
    }
    return int(lines.size()) * step;
}

void Canvas::image(int x, int y, const uint32_t* pixels, int width, int height) {
    if (width <= 0 || height <= 0) return;
    XImage image{};
    image.width = width;
    image.height = height;
    image.format = ZPixmap;
    // Cast away const: XPutImage does not write to the data, but the structure
    // it is handed has no way to say so.
    image.data = (char*)const_cast<uint32_t*>(pixels);
    image.byte_order = LSBFirst;
    image.bitmap_unit = 32;
    image.bitmap_bit_order = LSBFirst;
    image.bitmap_pad = 32;
    image.depth = 24;
    image.bytes_per_line = width * 4;
    image.bits_per_pixel = 32;
    image.red_mask = 0x00FF0000;
    image.green_mask = 0x0000FF00;
    image.blue_mask = 0x000000FF;
    if (!XInitImage(&image)) return;
    XPutImage(display_->xdisplay(), drawable_, gc_, &image, 0, 0, x, y, (unsigned)width,
              (unsigned)height);
}

// =============================================================================
// Window
// =============================================================================

Window::Window(Display* display) : display_(display) {}

Window::~Window() {
    cancelTimer();
    if (draw_) XftDrawDestroy(draw_);
    if (buffer_) XFreePixmap(display_->xdisplay(), buffer_);
    if (gc_) XFreeGC(display_->xdisplay(), gc_);
    if (window_) XDestroyWindow(display_->xdisplay(), window_);
    for (int fd : wakePipe_) {
        if (fd >= 0) ::close(fd);
    }
}

bool Window::create(const std::string& title, int width, int height) {
    ::Display* dpy = display_->xdisplay();
    width_ = width;
    height_ = height;

    XSetWindowAttributes attributes{};
    attributes.background_pixel = BlackPixel(dpy, display_->screen());
    attributes.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | ButtonPressMask |
                            ButtonReleaseMask | PointerMotionMask | LeaveWindowMask |
                            FocusChangeMask;
    window_ = XCreateWindow(dpy, RootWindow(dpy, display_->screen()), 0, 0, (unsigned)width,
                            (unsigned)height, 0, display_->depth(), InputOutput,
                            display_->visual(), CWBackPixel | CWEventMask, &attributes);
    if (!window_) return false;

    gc_ = XCreateGC(dpy, window_, 0, nullptr);
    setTitle(title);

    deleteWindow_ = display_->atom("WM_DELETE_WINDOW");
    XSetWMProtocols(dpy, window_, &deleteWindow_, 1);

    clipboardAtom_ = display_->atom("CLIPBOARD");
    utf8Atom_ = display_->atom("UTF8_STRING");
    targetsAtom_ = display_->atom("TARGETS");

    if (::pipe(wakePipe_) == 0) {
        for (int fd : wakePipe_) ::fcntl(fd, F_SETFL, O_NONBLOCK);
    }
    return true;
}

void Window::setTitle(const std::string& title) {
    if (!window_) return;
    ::Display* dpy = display_->xdisplay();
    XStoreName(dpy, window_, title.c_str());
    // _NET_WM_NAME as well, in UTF-8. XStoreName is Latin-1, so a path with an
    // accent in it arrives mangled in every modern task bar without this.
    XChangeProperty(dpy, window_, display_->atom("_NET_WM_NAME"),
                    display_->atom("UTF8_STRING"), 8, PropModeReplace,
                    (const unsigned char*)title.c_str(), int(title.size()));
}

void Window::setClass(const char* instance, const char* className) {
    if (!window_) return;
    XClassHint hint{};
    hint.res_name = const_cast<char*>(instance);
    hint.res_class = const_cast<char*>(className);
    XSetClassHint(display_->xdisplay(), window_, &hint);
}

void Window::setMinimumSize(int width, int height) {
    if (!window_) return;
    XSizeHints hints{};
    hints.flags = PMinSize;
    hints.min_width = width;
    hints.min_height = height;
    XSetWMNormalHints(display_->xdisplay(), window_, &hints);
}

void Window::show() {
    XMapWindow(display_->xdisplay(), window_);
    XFlush(display_->xdisplay());
}

void Window::close() { running_ = false; }
void Window::quit() { running_ = false; }

void Window::invalidate() { needsPaint_ = true; }

void Window::ensureBuffer() {
    if (buffer_ && bufferWidth_ == width_ && bufferHeight_ == height_) return;
    ::Display* dpy = display_->xdisplay();
    if (draw_) {
        XftDrawDestroy(draw_);
        draw_ = nullptr;
    }
    if (buffer_) XFreePixmap(dpy, buffer_);
    bufferWidth_ = std::max(1, width_);
    bufferHeight_ = std::max(1, height_);
    buffer_ = XCreatePixmap(dpy, window_, (unsigned)bufferWidth_, (unsigned)bufferHeight_,
                            (unsigned)display_->depth());
    draw_ = XftDrawCreate(dpy, buffer_, display_->visual(), display_->colormap());
}

void Window::paint() {
    needsPaint_ = false;
    ensureBuffer();
    if (!draw_) return;
    Canvas canvas(display_, buffer_, draw_, gc_, width_, height_);
    // A clip covering the window, pushed so that the GC and the Xft draw agree
    // from the first call rather than inheriting whatever the last paint left.
    canvas.pushClip(Rect{0, 0, width_, height_});
    if (onDraw) onDraw(canvas);
    canvas.popClip();
    XCopyArea(display_->xdisplay(), buffer_, window_, gc_, 0, 0, (unsigned)width_,
              (unsigned)height_, 0, 0);
    XFlush(display_->xdisplay());
}

void Window::setClipboardText(const std::string& text) {
    clipboard_ = text;
    if (!window_) return;
    XSetSelectionOwner(display_->xdisplay(), clipboardAtom_, window_, CurrentTime);
}

bool Window::capture(const std::string& path) {
    if (!buffer_ || width_ <= 0 || height_ <= 0) return false;
    XImage* image = XGetImage(display_->xdisplay(), buffer_, 0, 0, (unsigned)width_,
                              (unsigned)height_, AllPlanes, ZPixmap);
    if (!image) return false;

    // The masks come from the visual, not from the returned image.
    //
    // XGetImage is documented to fill in the image's own masks, and on Xwayland
    // it hands back an image with all three set to zero. Trusting them produced
    // a picture that was uniformly black while the pixels underneath were
    // perfectly correct - which is a maddening thing to debug, because every
    // check says the drawing worked and only the photograph disagrees.
    const unsigned long masks[3] = {display_->visual()->red_mask, display_->visual()->green_mask,
                                    display_->visual()->blue_mask};
    int shifts[3] = {0, 0, 0};
    for (int channel = 0; channel < 3; ++channel) {
        unsigned long mask = masks[channel];
        if (mask == 0) continue;
        while ((mask & 1) == 0) {
            mask >>= 1;
            ++shifts[channel];
        }
    }

    FILE* file = ::fopen(path.c_str(), "wb");
    if (!file) {
        XDestroyImage(image);
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width_, height_);
    std::vector<unsigned char> row(size_t(width_) * 3);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            // Through XGetPixel rather than by indexing the data directly: this
            // runs once, when somebody asked for a picture, so the version that
            // cannot be wrong about byte order is the right one.
            const unsigned long pixel = XGetPixel(image, x, y);
            for (int channel = 0; channel < 3; ++channel) {
                row[size_t(x) * 3 + size_t(channel)] =
                    (unsigned char)((pixel & masks[channel]) >> shifts[channel]);
            }
        }
        ::fwrite(row.data(), 1, row.size(), file);
    }
    ::fclose(file);
    XDestroyImage(image);
    return true;
}

int64_t Window::nowMillis() const {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    return int64_t(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void Window::post(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> guard(postedLock_);
        posted_.push_back(std::move(work));
    }
    if (wakePipe_[1] >= 0) {
        const char byte = 1;
        ssize_t ignored = ::write(wakePipe_[1], &byte, 1);
        (void)ignored;
    }
}

void Window::drainPosted() {
    std::vector<std::function<void()>> work;
    {
        std::lock_guard<std::mutex> guard(postedLock_);
        work.swap(posted_);
    }
    for (auto& item : work) item();
}

void Window::setTimer(int milliseconds, std::function<void()> work) {
    timerInterval_ = milliseconds;
    timerWork_ = std::move(work);
    timerNext_ = nowMillis() + milliseconds;
}

void Window::cancelTimer() {
    timerInterval_ = 0;
    timerWork_ = nullptr;
}

void Window::dispatch(XEvent* event) {
    switch (event->type) {
        case Expose:
            if (event->xexpose.count == 0) needsPaint_ = true;
            break;

        case ConfigureNotify: {
            const int width = event->xconfigure.width;
            const int height = event->xconfigure.height;
            if (width != width_ || height != height_) {
                width_ = width;
                height_ = height;
                if (onResize) onResize(width_, height_);
                needsPaint_ = true;
            }
            break;
        }

        case ButtonPress: {
            const XButtonEvent& source = event->xbutton;
            MouseEvent mouse;
            mouse.x = source.x;
            mouse.y = source.y;
            mouse.button = int(source.button);
            mouse.shift = (source.state & ShiftMask) != 0;
            mouse.control = (source.state & ControlMask) != 0;

            // Buttons four to seven are the wheel. X has no scroll event.
            if (source.button >= 4 && source.button <= 7) {
                int dx = 0;
                int dy = 0;
                if (source.button == 4) dy = -1;
                if (source.button == 5) dy = 1;
                if (source.button == 6) dx = -1;
                if (source.button == 7) dx = 1;
                if (onScroll) onScroll(mouse, dx, dy);
                break;
            }

            // Double clicks are ours to work out: X reports two presses and
            // leaves the question of whether they were one gesture to the client.
            const int64_t now = nowMillis();
            const bool near = std::abs(source.x - lastClickX_) < 5 &&
                              std::abs(source.y - lastClickY_) < 5;
            clickCount_ = (now - lastClickTime_ < 400 && near) ? clickCount_ + 1 : 1;
            lastClickTime_ = now;
            lastClickX_ = source.x;
            lastClickY_ = source.y;
            mouse.clicks = clickCount_;

            if (onMousePress) onMousePress(mouse);
            break;
        }

        case ButtonRelease: {
            if (event->xbutton.button >= 4 && event->xbutton.button <= 7) break;
            MouseEvent mouse;
            mouse.x = event->xbutton.x;
            mouse.y = event->xbutton.y;
            mouse.button = int(event->xbutton.button);
            mouse.clicks = clickCount_;
            if (onMouseRelease) onMouseRelease(mouse);
            break;
        }

        case MotionNotify: {
            // Only the newest position matters. Without this the loop redraws
            // once per motion event and falls behind the pointer on a fast move.
            XEvent latest = *event;
            while (XCheckTypedWindowEvent(display_->xdisplay(), window_, MotionNotify, &latest)) {
            }
            MouseEvent mouse;
            mouse.x = latest.xmotion.x;
            mouse.y = latest.xmotion.y;
            mouse.shift = (latest.xmotion.state & ShiftMask) != 0;
            mouse.control = (latest.xmotion.state & ControlMask) != 0;
            if (onMouseMove) onMouseMove(mouse);
            break;
        }

        case LeaveNotify:
            if (onMouseLeave) onMouseLeave();
            break;

        case KeyPress: {
            KeyEvent key;
            char buffer[32];
            KeySym symbol = 0;
            const int length = XLookupString(&event->xkey, buffer, sizeof(buffer), &symbol, nullptr);
            key.key = (unsigned long)symbol;
            if (length > 0) key.text.assign(buffer, size_t(length));
            key.shift = (event->xkey.state & ShiftMask) != 0;
            key.control = (event->xkey.state & ControlMask) != 0;
            if (onKey) onKey(key);
            break;
        }

        case ClientMessage:
            if ((Atom)event->xclient.data.l[0] == deleteWindow_) {
                if (onCloseRequested && !onCloseRequested()) break;
                if (onClose) onClose();
                running_ = false;
            }
            break;

        case SelectionRequest: {
            // Somebody is pasting. Answer with the text, or with the list of
            // formats when that is what was asked for.
            const XSelectionRequestEvent& request = event->xselectionrequest;
            XSelectionEvent reply{};
            reply.type = SelectionNotify;
            reply.requestor = request.requestor;
            reply.selection = request.selection;
            reply.target = request.target;
            reply.time = request.time;
            reply.property = None;

            if (request.target == targetsAtom_) {
                const Atom offered[] = {targetsAtom_, utf8Atom_, XA_STRING};
                XChangeProperty(display_->xdisplay(), request.requestor, request.property, XA_ATOM,
                                32, PropModeReplace, (const unsigned char*)offered, 3);
                reply.property = request.property;
            } else if (request.target == utf8Atom_ || request.target == XA_STRING) {
                XChangeProperty(display_->xdisplay(), request.requestor, request.property,
                                request.target, 8, PropModeReplace,
                                (const unsigned char*)clipboard_.data(), int(clipboard_.size()));
                reply.property = request.property;
            }
            XSendEvent(display_->xdisplay(), request.requestor, False, 0, (XEvent*)&reply);
            break;
        }

        case SelectionClear:
            // Something else owns the clipboard now, so this no longer speaks for
            // it and should not go on holding the text.
            clipboard_.clear();
            break;

        default:
            break;
    }
}

void Window::run() {
    ::Display* dpy = display_->xdisplay();
    const int connection = ConnectionNumber(dpy);
    running_ = true;

    while (running_) {
        drainPosted();

        while (running_ && XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            dispatch(&event);
        }
        if (!running_) break;

        drainPosted();

        if (timerInterval_ > 0 && timerWork_ && nowMillis() >= timerNext_) {
            timerNext_ = nowMillis() + timerInterval_;
            timerWork_();
        }

        if (needsPaint_) {
            paint();
            // Painting can leave more events queued; go round rather than
            // sleeping on a connection that already has something to say.
            if (XPending(dpy)) continue;
        }

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(connection, &readable);
        int highest = connection;
        if (wakePipe_[0] >= 0) {
            FD_SET(wakePipe_[0], &readable);
            highest = std::max(highest, wakePipe_[0]);
        }

        struct timeval timeout{};
        struct timeval* timeoutOrNever = nullptr;
        if (timerInterval_ > 0 && timerWork_) {
            const int64_t remaining = std::max<int64_t>(0, timerNext_ - nowMillis());
            timeout.tv_sec = remaining / 1000;
            timeout.tv_usec = (remaining % 1000) * 1000;
            timeoutOrNever = &timeout;
        }

        const int ready = ::select(highest + 1, &readable, nullptr, nullptr, timeoutOrNever);
        if (ready < 0) continue;
        if (wakePipe_[0] >= 0 && FD_ISSET(wakePipe_[0], &readable)) {
            char scratch[64];
            while (::read(wakePipe_[0], scratch, sizeof(scratch)) > 0) {
            }
        }
    }
}

}  // namespace ui

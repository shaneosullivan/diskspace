#pragma once

#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

/// The whole toolkit.
///
/// What replaced GTK, and it is deliberately not a widget library: this
/// application draws its own panes and always did, so what it needs from a
/// toolkit is a window, an event loop, a way to put pixels on the screen and a
/// way to measure and draw text. That is this file.
///
/// Everything here is Xlib plus Xft. Both are on any machine with an X session -
/// Xfce, GNOME and KDE all depend on them - which is the point: the application
/// is a few hundred kilobytes and asks the host for nothing it does not already
/// have. See docs/linux-x11-plan.md for what that decision cost and why it was
/// made anyway.
namespace ui {

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    bool contains(int px, int py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
    bool empty() const { return width <= 0 || height <= 0; }
    int right() const { return x + width; }
    int bottom() const { return y + height; }

    /// The overlap of two rectangles, which is empty when they do not touch.
    Rect intersect(const Rect& other) const;
    Rect inset(int amount) const {
        return Rect{x + amount, y + amount, width - 2 * amount, height - 2 * amount};
    }
};

/// 0xAARRGGBB, alpha first and opaque at 0xFF.
///
/// The low three bytes are in the same order as the treemap renderer's pixels
/// and as a Windows DIB, so an image produced by the shared code needs no
/// conversion on its way to the screen.
using Color = uint32_t;

constexpr Color rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (Color(r) << 16) | (Color(g) << 8) | Color(b);
}
constexpr Color rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (Color(a) << 24) | (Color(r) << 16) | (Color(g) << 8) | Color(b);
}
/// `over` drawn on `under`, both opaque out.
Color blend(Color under, Color over);

/// A font at one size. Owned by the Display; never constructed directly.
struct Font {
    XftFont* handle = nullptr;
    int ascent = 0;
    int descent = 0;
    int lineHeight = 0;
    /// Index into the Display's table, which is where the fallback faces found
    /// for this font are kept.
    int id = -1;
    explicit operator bool() const { return handle != nullptr; }
};

/// A stretch of text that one face can draw.
///
/// Text is split into these before it is measured or drawn, because one face is
/// rarely enough. The font a desktop calls "sans-serif" has no CJK in it, and
/// this application's whole job is showing filenames, which are in whatever
/// language the person who made them was using.
struct TextRun {
    size_t offset = 0;
    size_t length = 0;
    XftFont* font = nullptr;
};

enum class Weight { Regular, Bold };

/// Where a run of text sits inside the box it was given.
enum class Align { Left, Center, Right };

class Window;

/// The X connection, the fonts, and the scale everything is measured in.
class Display {
public:
    static Display* open();
    ~Display();

    ::Display* xdisplay() const { return display_; }
    int screen() const { return screen_; }
    Visual* visual() const { return visual_; }
    Colormap colormap() const { return colormap_; }
    int depth() const { return depth_; }

    /// Points scaled by the desktop's DPI, resolved once at startup.
    ///
    /// From the Xft.dpi X resource, which is what every desktop sets and what
    /// every toolkit reads. Absent, the screen's physical size is used, and
    /// failing that 96, which is the number that was standard when it stopped
    /// being true.
    double scale() const { return scale_; }
    int scaled(int pixels) const { return int(pixels * scale_ + 0.5); }

    /// A font at a size in points. Cached: asking twice gives the same face.
    Font font(double points, Weight weight = Weight::Regular);

    /// Split `utf8` into runs, each drawable by a single face.
    ///
    /// Characters the font has are kept in one run. For anything it does not,
    /// fontconfig is asked which installed face does, and the answer is cached
    /// against this font - so a directory of Japanese filenames costs one lookup
    /// rather than one per name.
    void splitRuns(const char* utf8, size_t length, const Font& font,
                   std::vector<TextRun>* out);

    Atom atom(const char* name);

private:
    Display() = default;
    bool init();

    struct CachedFont {
        double points;
        Weight weight;
        Font font;
        /// Faces found for characters the base font lacks, in the order they
        /// were needed. Short: a handful covers every script in practice.
        std::vector<XftFont*> fallbacks;
        /// Characters no installed font could draw, so the search is not repeated
        /// for every occurrence of the same missing glyph.
        std::vector<uint32_t> hopeless;
    };

    /// The face to draw `rune` in, or the base font when nothing has it.
    XftFont* faceFor(int fontId, uint32_t rune);

    ::Display* display_ = nullptr;
    int screen_ = 0;
    Visual* visual_ = nullptr;
    Colormap colormap_ = 0;
    int depth_ = 0;
    double scale_ = 1.0;
    std::vector<CachedFont> fonts_;
};

/// Somewhere to draw. Valid only for the duration of one paint.
class Canvas {
public:
    Canvas(Display* display, Drawable drawable, XftDraw* draw, GC gc, int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }
    Display* display() const { return display_; }

    void fill(const Rect& rect, Color color);
    /// A one-pixel border just inside `rect`.
    void frame(const Rect& rect, Color color);
    /// A filled rectangle with rounded corners, anti-aliased.
    void fillRounded(const Rect& rect, int radius, Color color);
    void line(int x1, int y1, int x2, int y2, Color color);

    /// Text, positioned by its baseline. Returns the width drawn.
    int text(int x, int baseline, const char* utf8, size_t length, const Font& font, Color color);
    int text(int x, int baseline, const std::string& s, const Font& font, Color color) {
        return text(x, baseline, s.data(), s.size(), font, color);
    }

    /// Text fitted to `box`, shortened with an ellipsis if it does not fit, and
    /// vertically centred. The common case, and the one worth having in one
    /// place: getting the ellipsis wrong is the kind of thing that only shows up
    /// on somebody else's filenames.
    void textIn(const Rect& box, const char* utf8, size_t length, const Font& font, Color color,
                Align align = Align::Left);
    void textIn(const Rect& box, const std::string& s, const Font& font, Color color,
                Align align = Align::Left) {
        textIn(box, s.data(), s.size(), font, color, align);
    }

    /// Split `text` into lines that each fit within `width`.
    ///
    /// Greedy, and it breaks at spaces. A single word longer than the line is
    /// left to overflow rather than broken mid-word: this draws prose, where that
    /// does not happen, and a hyphenation rule would be a lot of machinery for a
    /// case that does not arise.
    std::vector<std::string> wrap(const std::string& text, const Font& font, int width) const;

    /// Draw wrapped text from the top of `box`. Returns the height it used, so a
    /// caller can lay out whatever comes after it.
    int textBlock(const Rect& box, const std::string& text, const Font& font, Color color,
                  int lineSpacing = 2, Align align = Align::Left);

    int textWidth(const char* utf8, size_t length, const Font& font) const;
    int textWidth(const std::string& s, const Font& font) const {
        return textWidth(s.data(), s.size(), font);
    }

    /// A block of 0x00RRGGBB pixels, which is what the shared treemap renderer
    /// produces. Copied straight across on a little-endian machine.
    void image(int x, int y, const uint32_t* pixels, int width, int height);

    /// Drawing outside the clip is discarded. Restore with popClip, always.
    void pushClip(const Rect& rect);
    void popClip();
    /// The current clip, in window coordinates.
    Rect clip() const { return clips_.empty() ? Rect{0, 0, width_, height_} : clips_.back(); }

private:
    void applyClip();

    Display* display_ = nullptr;
    Drawable drawable_ = 0;
    XftDraw* draw_ = nullptr;
    GC gc_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::vector<Rect> clips_;
};

struct MouseEvent {
    int x = 0;
    int y = 0;
    /// 1 left, 2 middle, 3 right. Zero for movement.
    int button = 0;
    int clicks = 1;
    bool shift = false;
    bool control = false;
};

struct KeyEvent {
    /// An X keysym: XK_Return, XK_Up, and so on.
    unsigned long key = 0;
    /// The character typed, UTF-8, empty for keys that are not characters.
    std::string text;
    bool shift = false;
    bool control = false;
};

/// A top-level window, its backing store, and the event loop.
class Window {
public:
    Window(Display* display);
    ~Window();

    bool create(const std::string& title, int width, int height);
    void show();
    void close();

    void setTitle(const std::string& title);
    /// The name the desktop matches against the .desktop file, which is what
    /// puts the right icon in the task bar.
    void setClass(const char* instance, const char* className);
    void setMinimumSize(int width, int height);

    int width() const { return width_; }
    int height() const { return height_; }

    /// Repaint before the next idle. Coalesced, so calling it in a loop costs
    /// one redraw rather than one per call.
    void invalidate();

    /// Run until the window closes.
    void run();
    /// Stop the loop at the next opportunity.
    void quit();

    /// Run `work` on the UI thread, from any thread. The loop wakes for it.
    ///
    /// How the scan reaches the screen: the scanner runs on its own threads and
    /// must not touch X, since one connection is not safe to share. It posts
    /// instead, and the work happens between events like everything else.
    void post(std::function<void()> work);

    /// Call `work` every `milliseconds` on the UI thread. One timer per window,
    /// which is all the progress tick needs.
    void setTimer(int milliseconds, std::function<void()> work);
    void cancelTimer();

    std::function<void(Canvas&)> onDraw;
    std::function<void(const MouseEvent&)> onMousePress;
    std::function<void(const MouseEvent&)> onMouseRelease;
    std::function<void(const MouseEvent&)> onMouseMove;
    std::function<void()> onMouseLeave;
    /// Positive `dy` scrolls towards the end of the content.
    std::function<void(const MouseEvent&, int dx, int dy)> onScroll;
    std::function<void(const KeyEvent&)> onKey;
    std::function<void(int width, int height)> onResize;
    std::function<void()> onClose;
    /// Asked before the window closes. Return false to refuse.
    ///
    /// Separate from onClose, which is told after the decision is made. A window
    /// manager's close button is a request, and there are things - unfinished
    /// work, a question that has to be answered first - that are entitled to say
    /// no to it.
    std::function<bool()> onCloseRequested;

    ::Window handle() const { return window_; }
    Display* display() const { return display_; }

    /// Put `text` on the CLIPBOARD selection.
    ///
    /// X has no clipboard: it has selections, and the "clipboard" is whichever
    /// window currently claims to own one and is still running to answer for it.
    /// So this claims ownership and keeps the string, and the event loop answers
    /// requests for it until something else takes over. The consequence, which is
    /// how X has always worked, is that a copied path stops being available the
    /// moment this program exits, unless a clipboard manager took a copy.
    void setClipboardText(const std::string& text);

    /// Write what was last painted to a binary PPM file. True when it worked.
    ///
    /// Reading back the application's own backing pixmap rather than asking the
    /// screen, which is the only way that works everywhere: a compositor is not
    /// obliged to hand a window's contents to anybody, and under WSLg it will
    /// not. This asks the X server for a pixmap this process owns, so it needs
    /// no cooperation from the desktop and produces the same bytes whether the
    /// window is on top, buried, or off the side of the screen.
    ///
    /// PPM because it is a dozen lines and every tool reads it. Convert it
    /// afterwards if a PNG is wanted.
    bool capture(const std::string& path);

private:
    void ensureBuffer();
    void paint();
    void dispatch(XEvent* event);
    void drainPosted();
    int64_t nowMillis() const;

    Display* display_ = nullptr;
    ::Window window_ = 0;
    Pixmap buffer_ = 0;
    XftDraw* draw_ = nullptr;
    GC gc_ = 0;
    int width_ = 0;
    int height_ = 0;
    int bufferWidth_ = 0;
    int bufferHeight_ = 0;
    bool running_ = false;
    bool needsPaint_ = true;
    Atom deleteWindow_ = 0;

    /// What this window will hand over when something asks for the clipboard.
    std::string clipboard_;
    Atom clipboardAtom_ = 0;
    Atom utf8Atom_ = 0;
    Atom targetsAtom_ = 0;

    /// The self-pipe that lets another thread wake the loop. select() cannot
    /// wait on an X connection and a condition variable at once, and this is the
    /// usual answer: the writer makes the read end readable.
    int wakePipe_[2] = {-1, -1};
    std::mutex postedLock_;
    std::vector<std::function<void()>> posted_;

    int timerInterval_ = 0;
    int64_t timerNext_ = 0;
    std::function<void()> timerWork_;

    /// For synthesising double clicks, which X does not report.
    int64_t lastClickTime_ = 0;
    int lastClickX_ = 0;
    int lastClickY_ = 0;
    int clickCount_ = 0;
};

}  // namespace ui

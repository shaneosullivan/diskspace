#pragma once

#ifdef _WIN32
// For COLORREF, which the one Win32 accessor at the bottom returns. The layout
// itself is plain arithmetic and needs nothing.
#include <windows.h>
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "name_string.h"
#include "node_arena.h"

namespace app {

using diskscan::NameString;
using diskscan::NodeArena;
using diskscan::SizeMetric;

/// Plain double rectangle. Layout happens in device pixels at whatever scale the
/// window is running, so this deliberately is not an integer RECT.
struct RectD {
    double x = 0, y = 0, width = 0, height = 0;
    double minX() const { return x; }
    double minY() const { return y; }
    double maxX() const { return x + width; }
    double maxY() const { return y + height; }
    double midX() const { return x + width / 2; }
    double midY() const { return y + height / 2; }
    double area() const { return width * height; }
    bool contains(double px, double py) const {
        return px >= x && py >= y && px < x + width && py < y + height;
    }
};

/// Accumulated cushion surface: the quadratic height field z(x,y) built up by one
/// ridge per hierarchy level, per van Wijk & van de Wetering.
struct Cushion {
    double a = 0;  // x² coefficient
    double b = 0;  // y² coefficient
    double c = 0;  // x coefficient
    double d = 0;  // y coefficient

    /// Adds this rectangle's ridge to the surface. Called once per level as the
    /// recursion descends, so a leaf carries the sum of every enclosing folder's
    /// ridge — which is what makes directory structure visible without borders.
    void addRidge(const RectD& rect, double height) {
        if (rect.width <= 0 || rect.height <= 0) return;
        c += 4 * height * (rect.maxX() + rect.minX()) / rect.width;
        a -= 4 * height / rect.width;
        d += 4 * height * (rect.maxY() + rect.minY()) / rect.height;
        b -= 4 * height / rect.height;
    }
};

enum class LeafReason { File, NoChildren, DepthLimit, RectTooSmall, NoDrawableItems, Pooled };

struct TreemapCell {
    RectD rect;
    int32_t node = 0;
    int depth = 0;
    bool isLeaf = false;
    /// >0 when this cell stands in for that many items too small to draw
    /// individually. They are pooled rather than dropped, so no gap appears.
    int32_t aggregatedCount = 0;
    LeafReason leafReason = LeafReason::File;
    Cushion cushion;
};

/// A block drawn alongside the scanned folder's contents: free space on the
/// drive, or everything else in use on it.
struct ExtraBlock {
    int32_t id = 0;
    NameString label;
    int64_t bytes = 0;
    uint32_t colour = 0;
};

/// Stable colour per file kind, shared by the treemap and the kind table so the
/// two panes read as one system.
class KindPalette {
public:
    KindPalette();

    /// Derive colour from the kind itself, so a file type looks the same in every
    /// scan. Off, the biggest kinds in this scan get the most distinct hues.
    bool consistentColours = true;

    void setRanking(const std::vector<int16_t>& kindsBySize);
    uint32_t colour(int16_t kind) const;
#ifdef _WIN32
    /// The same colour in the byte order GDI wants, which is the reverse of the
    /// one everything else here uses.
    COLORREF gdiColour(int16_t kind) const {
        uint32_t c = colour(kind);
        return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
    }
#endif

private:
    std::vector<uint32_t> cache_;
    std::unordered_map<int16_t, int> rankOfKind_;
    static constexpr int kReservedSlots = 4;
};

struct TreemapOptions {
    SizeMetric metric = SizeMetric::Total;
    /// Show the length of file contents rather than the space occupied on disk.
    bool useLogicalSize = false;
    std::vector<ExtraBlock> extraBlocks;
};

/// A rendered treemap: the pixels to blit, plus a parallel buffer holding the
/// cell index at every pixel.
///
/// The id buffer is what makes hover instant. Hit testing is `ids[y * width + x]`
/// — one array read, no traversal, no matter how many rectangles are on screen.
/// Both buffers are produced together on a background thread.
struct TreemapImage {
    int width = 0;
    int height = 0;
    std::vector<TreemapCell> cells;
    std::vector<int32_t> ids;
    /// 0x00RRGGBB per pixel, which is exactly what a 32-bit BI_RGB device
    /// independent bitmap expects, so it blits with no conversion.
    std::vector<uint32_t> pixels;
    int32_t rootNode = 0;
    /// Rectangle drawn for each node — folders and files alike. Used both to ask
    /// how big a block really is on screen and to highlight whatever is selected
    /// in the file list.
    std::unordered_map<int32_t, RectD> nodeRects;
    std::unordered_map<int32_t, ExtraBlock> extraBlocks;

    int32_t cellIndexAt(int px, int py) const {
        if (px < 0 || py < 0 || px >= width || py >= height) return -1;
        return ids[size_t(py) * size_t(width) + size_t(px)];
    }
};

namespace Treemap {

/// Rectangles below this area are not worth drawing or recursing into. Rendering
/// far past the point of visual distinguishability is a large amount of work for
/// pixels nobody can tell apart.
inline constexpr double kMinimumSide = 3;
/// Only a stack-overflow backstop. The real stopping rule is the minimum size:
/// stop when a rectangle is too small to see. A tight depth limit instead made
/// deep trees bottom out early and render whole directories as flat blocks.
inline constexpr int kMaximumDepth = 64;
/// Every file gets its own rectangle down to this area. Only genuinely sub-pixel
/// items are pooled, and only so that no gap is left behind.
inline constexpr double kMinimumCellArea = 1;

/// The figure to draw a node at, honouring the physical/logical setting.
int64_t sizeOf(const NodeArena& arena, size_t node, const TreemapOptions& options);

/// Classic squarified layout: pack rows so cell aspect ratios stay near 1.
void layout(const std::vector<std::pair<int32_t, double>>& items, const RectD& bounds,
            bool preserveOrder, std::vector<std::pair<int32_t, RectD>>* out);

/// A colour for a directory we cannot recurse into. Follows the largest child
/// down until it reaches a file, so a collapsed folder takes on the colour of
/// whatever dominates it rather than rendering as an anonymous block.
int16_t representativeKind(const NodeArena& arena, int32_t node, SizeMetric metric);

/// Fills the pixel buffer and the id buffer in a single pass.
TreemapImage render(const NodeArena& arena, int32_t root, int width, int height,
                    const KindPalette& palette, const TreemapOptions& options);

}  // namespace Treemap
}  // namespace app

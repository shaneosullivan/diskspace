#include "treemap.h"

#include <algorithm>
#include <cmath>

#include "../core/file_kind.h"

namespace app {

namespace FileKind = diskscan::FileKind;

namespace {

// Cushion shading parameters.
/// Ridge height at the top level.
constexpr double kRidgeHeight = 0.38;
/// How much shallower each level's ridge is than its parent's.
///
/// This number decides whether the map reads as a hierarchy at all. At 0.7 a leaf
/// eight levels down keeps 0.7^8 ≈ 6% of the base ridge — effectively flat, so
/// whole folders melt into one smooth gradient and a folder's rectangle can cut
/// straight through what looks like a single block. At 0.91 the same leaf keeps
/// ≈ 47%, so every file still reads as its own cushion inside its folder's.
constexpr double kRidgeScaleFactor = 0.91;
/// Slight lift so cushion edges do not bottom out to black.
constexpr double kRenderGain = 1.04;
constexpr double kAmbientLight = 0.20;
/// Light direction, from the upper left and close to head on.
///
/// A steeply angled light drives the cosine to zero across most of every cushion,
/// so large areas fall into flat shadow and lose their shape. Keeping the light
/// near the viewer preserves the dome on every block.
constexpr double kLightXY = -2.5;
const double kLightLength = std::sqrt(kLightXY * kLightXY * 2 + 100);
const double kLightX = kLightXY / kLightLength;
const double kLightY = kLightXY / kLightLength;
const double kLightZ = 10.0 / kLightLength;

/// Sentinel node id for the cell that stands in for everything too small to draw.
constexpr int32_t kPooledSentinel = INT32_MIN;

double worstAspect(const std::vector<std::pair<int32_t, double>>& items, size_t begin, size_t end,
                   double sum, double shortSide, double scale) {
    double thickness = (sum * scale) / shortSide;
    if (thickness <= 0) return HUGE_VAL;
    double worst = 0;
    for (size_t i = begin; i <= end; ++i) {
        double length = (items[i].second * scale) / thickness;
        if (length <= 0) return HUGE_VAL;
        worst = (std::max)(worst, (std::max)(thickness / length, length / thickness));
    }
    return worst;
}

uint32_t hsbToRgb(double hue, double saturation, double brightness) {
    double r = brightness, g = brightness, b = brightness;
    if (saturation > 0) {
        double h = hue * 6.0;
        int sector = int(std::floor(h)) % 6;
        double f = h - std::floor(h);
        double p = brightness * (1 - saturation);
        double q = brightness * (1 - saturation * f);
        double t = brightness * (1 - saturation * (1 - f));
        switch (sector) {
            case 0: r = brightness; g = t; b = p; break;
            case 1: r = q; g = brightness; b = p; break;
            case 2: r = p; g = brightness; b = t; break;
            case 3: r = p; g = q; b = brightness; break;
            case 4: r = t; g = p; b = brightness; break;
            default: r = brightness; g = p; b = q; break;
        }
    }
    auto channel = [](double v) { return uint32_t((std::min)(255.0, (std::max)(0.0, v * 255.0))); };
    return 0xFF000000u | (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

/// Fills one cell, lighting its accumulated cushion surface.
///
/// Because the surface carries a ridge from every enclosing folder, the shading
/// itself shows the directory structure — which is what borders would otherwise
/// have to stand in for.
void paint(const TreemapCell& cell, uint32_t colour, int index, uint32_t* pixels, int32_t* ids,
           int width, int height) {
    // Round both edges the same way so cells tile exactly. Expanding outward
    // (floor then ceil) makes neighbours overlap.
    int x0 = (std::max)(0, int(std::lround(cell.rect.minX())));
    int y0 = (std::max)(0, int(std::lround(cell.rect.minY())));
    int x1 = (std::min)(width, int(std::lround(cell.rect.maxX())));
    int y1 = (std::min)(height, int(std::lround(cell.rect.maxY())));
    if (x1 <= x0 || y1 <= y0) return;

    const double baseR = double((colour >> 16) & 0xFF);
    const double baseG = double((colour >> 8) & 0xFF);
    const double baseB = double(colour & 0xFF);
    const Cushion& cushion = cell.cushion;
    constexpr double diffuse = 1 - kAmbientLight;

    for (int y = y0; y < y1; ++y) {
        size_t row = size_t(y) * size_t(width);
        double ny = -(2 * cushion.b * (double(y) + 0.5) + cushion.d);
        for (int x = x0; x < x1; ++x) {
            double nx = -(2 * cushion.a * (double(x) + 0.5) + cushion.c);
            double cosa = (nx * kLightX + ny * kLightY + kLightZ) /
                          std::sqrt(nx * nx + ny * ny + 1);
            if (cosa < 0) cosa = 0;
            // Slight gain so mid-tones do not sit dark; a steep surface otherwise
            // leaves most of a large cushion dim.
            double intensity = (std::min)(1.0, (kAmbientLight + diffuse * cosa) * kRenderGain);
            uint32_t r = uint32_t((std::min)(255.0, baseR * intensity));
            uint32_t g = uint32_t((std::min)(255.0, baseG * intensity));
            uint32_t b = uint32_t((std::min)(255.0, baseB * intensity));
            pixels[row + size_t(x)] = (r << 16) | (g << 8) | b;
            ids[row + size_t(x)] = int32_t(index);
        }
    }
}

void recurse(const NodeArena& arena, int32_t node, const RectD& rect, int depth,
             const TreemapOptions& options, const Cushion& inherited, double ridge,
             std::vector<TreemapCell>* cells) {
    // Every level contributes its own ridge on top of its ancestors'.
    Cushion cushion = inherited;
    if (depth > 0) cushion.addRidge(rect, ridge);
    const double childRidge = ridge * kRidgeScaleFactor;
    const size_t i = size_t(uint32_t(node));
    auto children = arena.children(i);

    // Area, not side length. A 2x500 sliver is 1000 visible pixels; bailing on it
    // paints a large folder-coloured block where real content should be.
    const bool tooSmall = rect.width < 1 || rect.height < 1 ||
                          rect.area() < Treemap::kMinimumSide * Treemap::kMinimumSide;

    if (children.empty() || depth >= Treemap::kMaximumDepth || tooSmall) {
        LeafReason reason = children.empty()
                                ? LeafReason::NoChildren
                                : (depth >= Treemap::kMaximumDepth ? LeafReason::DepthLimit
                                                                   : LeafReason::RectTooSmall);
        TreemapCell cell;
        cell.rect = rect;
        cell.node = node;
        cell.depth = depth;
        cell.isLeaf = true;
        cell.leafReason = reason;
        cell.cushion = cushion;
        cells->push_back(cell);
        return;
    }

    std::vector<std::pair<int32_t, double>> items;
    items.reserve(children.size() + (depth == 0 ? options.extraBlocks.size() : 0));
    if (depth == 0) {
        for (const ExtraBlock& block : options.extraBlocks) {
            if (block.bytes > 0) items.emplace_back(block.id, double(block.bytes));
        }
    }
    for (size_t c = children.begin; c < children.end; ++c) {
        double v = double(Treemap::sizeOf(arena, c, options));
        if (v > 0) items.emplace_back(int32_t(c), v);
    }
    if (items.empty()) {
        TreemapCell cell;
        cell.rect = rect;
        cell.node = node;
        cell.depth = depth;
        cell.isLeaf = true;
        cell.leafReason = LeafReason::NoDrawableItems;
        cell.cushion = cushion;
        cells->push_back(cell);
        return;
    }

    // No inset. Blocks are told apart by their cushion shading rather than by gaps
    // or outlines, so the map tiles completely and no background shows through.
    const RectD& inner = rect;

    // Pool the items too small to render into one cell, instead of dropping them.
    // Dropping leaves their area showing the background through as noise.
    double totalValue = 0;
    for (const auto& item : items) totalValue += item.second;
    const double scale = inner.area() / (std::max)(totalValue, 1.0);
    std::vector<std::pair<int32_t, double>> drawable;
    drawable.reserve(items.size());
    double pooledValue = 0;
    int32_t pooledCount = 0;
    int32_t pooledKind = -1;
    for (const auto& item : items) {
        if (item.second * scale >= Treemap::kMinimumCellArea) {
            drawable.push_back(item);
        } else {
            if (pooledKind < 0) pooledKind = item.first;
            pooledValue += item.second;
            ++pooledCount;
        }
    }

    // Order the children so that files of the same kind sit together.
    //
    // Laid out purely by size, files of one kind end up scattered and a folder
    // boundary can fall in the middle of an apparently uniform area. Grouping by
    // kind makes a folder's contents read as contiguous bands of colour, so its
    // edge lands where the colour changes. Groups are still ordered largest
    // first, which is what keeps the squarified aspect ratios reasonable.
    {
        std::unordered_map<int32_t, double> groupTotals;
        std::unordered_map<int32_t, std::vector<std::pair<int32_t, double>>> groupItems;
        for (const auto& item : drawable) {
            int32_t key;
            if (item.first < 0) {
                key = item.first;  // free / other space: its own group
            } else {
                size_t index = size_t(uint32_t(item.first));
                // Each directory is its own group: it is a nested map, not a colour.
                key = arena.isDirectory(index) ? -(item.first + 1) : int32_t(arena.kind[index]);
            }
            groupTotals[key] += item.second;
            groupItems[key].push_back(item);
        }
        std::vector<int32_t> keys;
        keys.reserve(groupTotals.size());
        for (const auto& g : groupTotals) keys.push_back(g.first);
        std::sort(keys.begin(), keys.end(),
                  [&](int32_t a, int32_t b) { return groupTotals[a] > groupTotals[b]; });
        std::vector<std::pair<int32_t, double>> ordered;
        ordered.reserve(drawable.size());
        for (int32_t key : keys) {
            auto& group = groupItems[key];
            std::sort(group.begin(), group.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            ordered.insert(ordered.end(), group.begin(), group.end());
        }
        drawable.swap(ordered);
    }

    // Tagged with a sentinel rather than identified by position: the layout
    // re-sorts, so index or value matching afterwards would be unreliable.
    if (pooledCount > 0 && pooledValue * scale >= 1) {
        drawable.emplace_back(kPooledSentinel, pooledValue);
    }
    if (drawable.empty()) {
        TreemapCell cell;
        cell.rect = rect;
        cell.node = node;
        cell.depth = depth;
        cell.isLeaf = true;
        cell.leafReason = LeafReason::NoDrawableItems;
        cell.cushion = cushion;
        cells->push_back(cell);
        return;
    }

    TreemapCell container;
    container.rect = rect;
    container.node = node;
    container.depth = depth;
    container.isLeaf = false;
    container.cushion = cushion;
    cells->push_back(container);

    std::vector<std::pair<int32_t, RectD>> placed;
    Treemap::layout(drawable, inner, true, &placed);
    for (const auto& entry : placed) {
        int32_t child = entry.first;
        const RectD& childRect = entry.second;
        if (child == kPooledSentinel) {
            // Coloured by the largest item it stands for, so the pooled block
            // still reads as belonging to whatever dominates there.
            TreemapCell cell;
            cell.rect = childRect;
            cell.node = pooledKind;
            cell.depth = depth + 1;
            cell.isLeaf = true;
            cell.aggregatedCount = pooledCount;
            cell.leafReason = LeafReason::Pooled;
            cell.cushion = cushion;
            cell.cushion.addRidge(childRect, childRidge);
            cells->push_back(cell);
            continue;
        }
        if (child < 0) {
            TreemapCell cell;
            cell.rect = childRect;
            cell.node = child;
            cell.depth = depth + 1;
            cell.isLeaf = true;
            cell.cushion = cushion;
            cell.cushion.addRidge(childRect, childRidge);
            cells->push_back(cell);
            continue;
        }
        if (arena.isDirectory(size_t(uint32_t(child)))) {
            recurse(arena, child, childRect, depth + 1, options, cushion, childRidge, cells);
        } else {
            TreemapCell cell;
            cell.rect = childRect;
            cell.node = child;
            cell.depth = depth + 1;
            cell.isLeaf = true;
            cell.cushion = cushion;
            cell.cushion.addRidge(childRect, childRidge);
            cells->push_back(cell);
        }
    }
}

}  // namespace

// MARK: - Palette

KindPalette::KindPalette() {
    FileKind::warmUp();
    int total = FileKind::count();
    cache_.reserve(size_t(total));
    for (int index = 0; index < total; ++index) {
        if (index == FileKind::Directory) {
            cache_.push_back(0xFF3A3A3C);
            continue;
        }
        if (index == FileKind::Symlink) {
            cache_.push_back(0xFF5A5A5E);
            continue;
        }
        // Golden-ratio hue stepping keeps adjacent kinds visually distinct instead
        // of producing the near-identical neighbours a linear ramp gives.
        double hue = std::fmod(double(index) * 0.6180339887, 1.0);
        double saturation = 0.55 + 0.2 * double(index % 3) / 2.0;
        double brightness = 0.72 + 0.18 * double(index % 2);
        cache_.push_back(hsbToRgb(hue, saturation, brightness));
    }
}

void KindPalette::setRanking(const std::vector<int16_t>& kindsBySize) {
    rankOfKind_.clear();
    for (size_t i = 0; i < kindsBySize.size(); ++i) rankOfKind_[kindsBySize[i]] = int(i);
}

uint32_t KindPalette::colour(int16_t kind) const {
    if (!consistentColours && int(cache_.size()) > kReservedSlots) {
        auto it = rankOfKind_.find(kind);
        if (it != rankOfKind_.end()) {
            int span = int(cache_.size()) - kReservedSlots;
            return cache_[size_t(kReservedSlots + (it->second % span))];
        }
    }
    size_t i = size_t(kind);
    return (kind >= 0 && i < cache_.size()) ? cache_[i] : 0xFF808080;
}

// MARK: - Layout

namespace Treemap {

int64_t sizeOf(const NodeArena& arena, size_t node, const TreemapOptions& options) {
    // Only the total has both forms; the recency figures are accumulated from
    // occupied space alone.
    if (options.useLogicalSize && options.metric == SizeMetric::Total) return arena.logical[node];
    return arena.size(options.metric, node);
}

void layout(const std::vector<std::pair<int32_t, double>>& source, const RectD& bounds,
            bool preserveOrder, std::vector<std::pair<int32_t, RectD>>* out) {
    out->clear();
    std::vector<std::pair<int32_t, double>> items;
    items.reserve(source.size());
    for (const auto& item : source) {
        if (item.second > 0) items.push_back(item);
    }
    if (items.empty() || bounds.width <= 0 || bounds.height <= 0) return;
    if (!preserveOrder) {
        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    double total = 0;
    for (const auto& item : items) total += item.second;
    RectD rect = bounds;
    size_t i = 0;

    while (i < items.size() && total > 0 && rect.width > 0 && rect.height > 0) {
        double shortSide = (std::min)(rect.width, rect.height);
        double scale = rect.area() / total;

        double rowSum = 0;
        size_t rowEnd = i;
        double bestRatio = HUGE_VAL;
        while (rowEnd < items.size()) {
            double candidateSum = rowSum + items[rowEnd].second;
            double ratio = worstAspect(items, i, rowEnd, candidateSum, shortSide, scale);
            if (ratio > bestRatio) break;
            bestRatio = ratio;
            rowSum = candidateSum;
            ++rowEnd;
        }
        if (rowEnd == i) {
            rowEnd = i + 1;
            rowSum = items[i].second;
        }

        double thickness = (rowSum * scale) / shortSide;
        double offset = 0;
        const bool horizontal = rect.width >= rect.height;
        for (size_t j = i; j < rowEnd; ++j) {
            double cellLength = (items[j].second * scale) / (std::max)(thickness, 1e-9);
            RectD r;
            if (horizontal) {
                r = {rect.minX(), rect.minY() + offset, thickness, cellLength};
            } else {
                r = {rect.minX() + offset, rect.minY(), cellLength, thickness};
            }
            out->emplace_back(items[j].first, r);
            offset += cellLength;
        }

        if (horizontal) {
            rect = {rect.minX() + thickness, rect.minY(), rect.width - thickness, rect.height};
        } else {
            rect = {rect.minX(), rect.minY() + thickness, rect.width, rect.height - thickness};
        }
        total -= rowSum;
        i = rowEnd;
    }
}

int16_t representativeKind(const NodeArena& arena, int32_t node, SizeMetric metric) {
    size_t current = size_t(uint32_t(node));
    int steps = 0;
    // Both the descent and the fan-out are capped so this stays cheap enough to
    // call once per cell.
    while (arena.isDirectory(current) && steps < 8) {
        auto kids = arena.children(current);
        if (kids.empty()) break;
        size_t best = size_t(-1);
        int64_t bestValue = -1;
        int scanned = 0;
        for (size_t c = kids.begin; c < kids.end; ++c) {
            if (scanned >= 256) break;
            ++scanned;
            int64_t v = arena.size(metric, c);
            if (v > bestValue) {
                bestValue = v;
                best = c;
            }
        }
        if (best == size_t(-1) || bestValue <= 0) break;
        current = best;
        ++steps;
    }
    return arena.kind[current];
}

TreemapImage render(const NodeArena& arena, int32_t root, int width, int height,
                    const KindPalette& palette, const TreemapOptions& options) {
    TreemapImage image;
    image.rootNode = root;
    // Readers must never touch a node at or above the published count.
    if (width <= 0 || height <= 0 || size_t(uint32_t(root)) >= arena.count()) return image;

    image.width = width;
    image.height = height;
    recurse(arena, root, RectD{0, 0, double(width), double(height)}, 0, options, Cushion(),
            kRidgeHeight, &image.cells);

    for (const ExtraBlock& block : options.extraBlocks) image.extraBlocks[block.id] = block;

    image.pixels.assign(size_t(width) * size_t(height), 0x001C1C1E);
    image.ids.assign(size_t(width) * size_t(height), -1);

    uint32_t* pixels = image.pixels.data();
    int32_t* ids = image.ids.data();
    for (size_t index = 0; index < image.cells.size(); ++index) {
        const TreemapCell& cell = image.cells[index];
        if (!cell.isLeaf) continue;
        auto extra = image.extraBlocks.find(cell.node);
        if (extra != image.extraBlocks.end()) {
            paint(cell, extra->second.colour, int(index), pixels, ids, width, height);
            continue;
        }
        int16_t kind = arena.kind[size_t(uint32_t(cell.node))];
        if (kind == FileKind::Directory) {
            kind = representativeKind(arena, cell.node, options.metric);
        }
        // A folder is always shown as its contents. If even the representative
        // lookup found nothing, fall back to a normal colour rather than painting
        // an anonymous hole.
        if (kind == FileKind::Directory) kind = FileKind::otherId();
        paint(cell, palette.colour(kind), int(index), pixels, ids, width, height);
    }

    image.nodeRects.reserve(image.cells.size());
    for (const TreemapCell& cell : image.cells) {
        // Pooled cells stand for many items under one representative node, so
        // their rectangle does not belong to that node and would highlight the
        // wrong area.
        if (cell.aggregatedCount != 0 || cell.node < 0) continue;
        auto existing = image.nodeRects.find(cell.node);
        // Container cells carry the folder's full rectangle; prefer them, and
        // otherwise keep the largest rectangle seen for that node.
        if (existing != image.nodeRects.end() && existing->second.area() >= cell.rect.area()) {
            continue;
        }
        image.nodeRects[cell.node] = cell.rect;
    }
    return image;
}

}  // namespace Treemap
}  // namespace app

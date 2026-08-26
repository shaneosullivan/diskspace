#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "../core/node_arena.h"
#include "ui_common.h"

namespace app {

using diskscan::NodeArena;

/// Floating window describing the current selection.
///
/// The scanner keeps only what a treemap needs, so ownership, attributes and
/// modification date are read with a single call when the panel is shown. That is
/// one lookup per selection rather than extra bytes per node across millions.
class InformationPanel {
public:
    static InformationPanel& shared();
    void show(HWND owner);
    void hide();
    bool visible() const;
    void update(const NodeArena& arena, size_t node, const std::wstring& sizeText);
    void clear();

private:
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    void ensureWindow(HWND owner);
    void paint(HDC dc);

    HWND window_ = nullptr;
    std::vector<std::pair<std::wstring, std::wstring>> fields_;
    std::wstring title_;
    ui::BackBuffer buffer_;
};

/// Lists the chain of folders enclosing the current selection, with each one's
/// size, so the path being explored can be read at a glance and any level jumped
/// back to.
class SelectionPanel {
public:
    static SelectionPanel& shared();
    void show(HWND owner);
    void hide();
    bool visible() const;
    void update(const NodeArena& arena, int32_t node,
                const std::function<std::wstring(size_t)>& sizeText);
    void clear();

    std::function<void(int32_t)> onChoose;

private:
    struct Entry {
        int32_t node;
        std::wstring name;
        std::wstring size;
        int depth;
    };

    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM w, LPARAM l);
    void ensureWindow(HWND owner);
    void paint(HDC dc);
    int rowAt(int y) const;

    HWND window_ = nullptr;
    std::vector<Entry> entries_;
    int hovered_ = -1;
    ui::BackBuffer buffer_;
};

}  // namespace app

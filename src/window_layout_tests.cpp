#include "window_layout.h"

#include <array>
#include <cstdlib>
#include <iostream>

namespace {

constexpr int kTitleHeight = 38;
constexpr int kContentWidth = 1400;
constexpr int kNormalContentHeight = 608;

bool HasExpectedAspect(const keypulse::PixelRectangle& rectangle) {
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    return height == keypulse::WindowHeightForWidth(
                         width, kTitleHeight, kContentWidth, kNormalContentHeight);
}

} // namespace

int main() {
    if (keypulse::WindowHeightForWidth(
            1400, kTitleHeight, kContentWidth, kNormalContentHeight) != 646) return 10;
    // The content doubles, while the title bar remains exactly 38 pixels tall.
    if (keypulse::WindowHeightForWidth(
            2800, kTitleHeight, kContentWidth, kNormalContentHeight) != 1254) return 11;
    if (keypulse::WindowWidthForHeight(
            1254, kTitleHeight, kContentWidth, kNormalContentHeight) != 2800) return 12;

    constexpr std::array edges{
        keypulse::SizingEdge::Left,
        keypulse::SizingEdge::Right,
        keypulse::SizingEdge::Top,
        keypulse::SizingEdge::TopLeft,
        keypulse::SizingEdge::TopRight,
        keypulse::SizingEdge::Bottom,
        keypulse::SizingEdge::BottomLeft,
        keypulse::SizingEdge::BottomRight,
    };
    for (const auto edge : edges) {
        keypulse::PixelRectangle rectangle{100, 200, 1720, 930};
        const auto original = rectangle;
        keypulse::ConstrainSizingRectangle(
            rectangle, edge, kTitleHeight, kContentWidth, kNormalContentHeight);
        if (!HasExpectedAspect(rectangle)) return 20 + static_cast<int>(edge);

        switch (edge) {
        case keypulse::SizingEdge::Left:
            if (rectangle.right != original.right ||
                std::abs((rectangle.top + rectangle.bottom) -
                         (original.top + original.bottom)) > 1) return 31;
            break;
        case keypulse::SizingEdge::Right:
            if (rectangle.left != original.left ||
                std::abs((rectangle.top + rectangle.bottom) -
                         (original.top + original.bottom)) > 1) return 32;
            break;
        case keypulse::SizingEdge::Top:
            if (rectangle.bottom != original.bottom ||
                std::abs((rectangle.left + rectangle.right) -
                         (original.left + original.right)) > 1) return 33;
            break;
        case keypulse::SizingEdge::TopLeft:
            if (rectangle.right != original.right || rectangle.bottom != original.bottom) return 34;
            break;
        case keypulse::SizingEdge::TopRight:
            if (rectangle.left != original.left || rectangle.bottom != original.bottom) return 35;
            break;
        case keypulse::SizingEdge::Bottom:
            if (rectangle.top != original.top ||
                std::abs((rectangle.left + rectangle.right) -
                         (original.left + original.right)) > 1) return 36;
            break;
        case keypulse::SizingEdge::BottomLeft:
            if (rectangle.right != original.right || rectangle.top != original.top) return 37;
            break;
        case keypulse::SizingEdge::BottomRight:
            if (rectangle.left != original.left || rectangle.top != original.top) return 38;
            break;
        }
    }

    std::cout << "window layout tests passed\n";
    return 0;
}

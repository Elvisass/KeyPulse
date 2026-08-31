#include "window_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace keypulse {
namespace {

void SetHeight(PixelRectangle& rectangle, SizingEdge edge, int height) noexcept {
    switch (edge) {
    case SizingEdge::Top:
    case SizingEdge::TopLeft:
    case SizingEdge::TopRight:
        rectangle.top = rectangle.bottom - height;
        break;
    case SizingEdge::Bottom:
    case SizingEdge::BottomLeft:
    case SizingEdge::BottomRight:
        rectangle.bottom = rectangle.top + height;
        break;
    default: {
        const int doubledCenter = rectangle.top + rectangle.bottom;
        rectangle.top = (doubledCenter - height) / 2;
        rectangle.bottom = rectangle.top + height;
        break;
    }
    }
}

void SetWidth(PixelRectangle& rectangle, SizingEdge edge, int width) noexcept {
    switch (edge) {
    case SizingEdge::Left:
    case SizingEdge::TopLeft:
    case SizingEdge::BottomLeft:
        rectangle.left = rectangle.right - width;
        break;
    case SizingEdge::Right:
    case SizingEdge::TopRight:
    case SizingEdge::BottomRight:
        rectangle.right = rectangle.left + width;
        break;
    default: {
        const int doubledCenter = rectangle.left + rectangle.right;
        rectangle.left = (doubledCenter - width) / 2;
        rectangle.right = rectangle.left + width;
        break;
    }
    }
}

} // namespace

int WindowHeightForWidth(int width, int titleHeight,
                         int contentWidth, int contentHeight) noexcept {
    const int safeWidth = std::max(1, width);
    const int safeContentWidth = std::max(1, contentWidth);
    const int safeContentHeight = std::max(1, contentHeight);
    const auto scaledContentHeight = static_cast<int>(std::lround(
        static_cast<double>(safeWidth) * safeContentHeight / safeContentWidth));
    return std::max(0, titleHeight) + std::max(1, scaledContentHeight);
}

int WindowWidthForHeight(int height, int titleHeight,
                         int contentWidth, int contentHeight) noexcept {
    const int availableHeight = std::max(1, height - std::max(0, titleHeight));
    const int safeContentWidth = std::max(1, contentWidth);
    const int safeContentHeight = std::max(1, contentHeight);
    return std::max(1, static_cast<int>(std::lround(
        static_cast<double>(availableHeight) * safeContentWidth / safeContentHeight)));
}

void ConstrainSizingRectangle(PixelRectangle& rectangle, SizingEdge edge,
                              int titleHeight, int contentWidth,
                              int contentHeight) noexcept {
    const int width = std::max(1, rectangle.right - rectangle.left);
    const int height = std::max(1, rectangle.bottom - rectangle.top);
    const int heightFromWidth = WindowHeightForWidth(
        width, titleHeight, contentWidth, contentHeight);
    const int widthFromHeight = WindowWidthForHeight(
        height, titleHeight, contentWidth, contentHeight);

    switch (edge) {
    case SizingEdge::Left:
    case SizingEdge::Right:
        SetHeight(rectangle, edge, heightFromWidth);
        break;
    case SizingEdge::Top:
    case SizingEdge::Bottom:
        SetWidth(rectangle, edge, widthFromHeight);
        break;
    case SizingEdge::TopLeft:
    case SizingEdge::TopRight:
    case SizingEdge::BottomLeft:
    case SizingEdge::BottomRight:
        if (std::abs(heightFromWidth - height) <= std::abs(widthFromHeight - width)) {
            SetHeight(rectangle, edge, heightFromWidth);
        } else {
            SetWidth(rectangle, edge, widthFromHeight);
        }
        break;
    }
}

} // namespace keypulse

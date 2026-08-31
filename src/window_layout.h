#pragma once

#include <cstdint>

namespace keypulse {

// Values intentionally match the Win32 WMSZ_* constants.
enum class SizingEdge : std::uint32_t {
    Left = 1,
    Right = 2,
    Top = 3,
    TopLeft = 4,
    TopRight = 5,
    Bottom = 6,
    BottomLeft = 7,
    BottomRight = 8,
};

struct PixelRectangle {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

[[nodiscard]] int WindowHeightForWidth(int width, int titleHeight,
                                       int contentWidth, int contentHeight) noexcept;
[[nodiscard]] int WindowWidthForHeight(int height, int titleHeight,
                                       int contentWidth, int contentHeight) noexcept;

void ConstrainSizingRectangle(PixelRectangle& rectangle, SizingEdge edge,
                              int titleHeight, int contentWidth,
                              int contentHeight) noexcept;

} // namespace keypulse

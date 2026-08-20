#include "WindowGeometry.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace liney {

WindowRect clampWindowToWorkArea(WindowRect window, WindowRect workArea) {
    const int64_t right = static_cast<int64_t>(workArea.x) + workArea.width;
    const int64_t bottom = static_cast<int64_t>(workArea.y) + workArea.height;
    int64_t x = window.x;
    int64_t y = window.y;
    if (x + window.width > right) x = right - window.width;
    if (y + window.height > bottom) y = bottom - window.height;
    if (x < workArea.x) x = workArea.x;
    if (y < workArea.y) y = workArea.y;
    window.x = static_cast<int>(std::clamp<int64_t>(
        x, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    window.y = static_cast<int>(std::clamp<int64_t>(
        y, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    return window;
}

} // namespace liney

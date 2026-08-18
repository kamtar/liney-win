#include "app/ResponsiveLayout.h"

#include <algorithm>

namespace liney {

ResponsivePanelLayout layoutResponsivePanels(
    float totalWidth, bool showLeft, bool showRight, float desiredPanelWidth,
    float compactPanelWidth, float minimumTerminalWidth) {
    return layoutResponsivePanels(
        totalWidth, showLeft, showRight, desiredPanelWidth, desiredPanelWidth,
        compactPanelWidth, compactPanelWidth, minimumTerminalWidth);
}

ResponsivePanelLayout layoutResponsivePanels(
    float totalWidth, bool showLeft, bool showRight,
    float leftDesiredWidth, float rightDesiredWidth,
    float leftCompactWidth, float rightCompactWidth,
    float minimumTerminalWidth) {
    ResponsivePanelLayout out;
    totalWidth = std::max(0.0f, totalWidth);
    leftDesiredWidth = std::max(0.0f, leftDesiredWidth);
    rightDesiredWidth = std::max(0.0f, rightDesiredWidth);
    leftCompactWidth =
        std::clamp(leftCompactWidth, 0.0f, leftDesiredWidth);
    rightCompactWidth =
        std::clamp(rightCompactWidth, 0.0f, rightDesiredWidth);
    minimumTerminalWidth =
        std::clamp(minimumTerminalWidth, 0.0f, totalWidth);

    float budget = std::max(0.0f, totalWidth - minimumTerminalWidth);
    if (showLeft && showRight) {
        if (budget >= leftDesiredWidth + rightDesiredWidth) {
            out.leftWidth = leftDesiredWidth;
            out.rightWidth = rightDesiredWidth;
        } else if (budget >= leftCompactWidth + rightCompactWidth) {
            // Preserve each panel's remembered preference while shrinking
            // both toward their readable compact widths.
            const float leftFlex = leftDesiredWidth - leftCompactWidth;
            const float rightFlex = rightDesiredWidth - rightCompactWidth;
            const float flex = leftFlex + rightFlex;
            const float compactBudget =
                budget - leftCompactWidth - rightCompactWidth;
            const float leftShare =
                flex > 0.0f ? compactBudget * leftFlex / flex
                            : compactBudget * 0.5f;
            out.leftWidth = leftCompactWidth + leftShare;
            out.rightWidth = budget - out.leftWidth;
            out.leftCompact = out.rightCompact = true;
        } else if (budget >= leftCompactWidth) {
            // The workspace carries repository/task context; retain it and
            // collapse the optional file navigator first.
            out.leftWidth = std::min(leftDesiredWidth, budget);
            out.leftCompact = out.leftWidth < leftDesiredWidth;
            out.rightCompact = true;
        } else {
            // A sliver of panel is worse than no panel: its labels are clipped
            // and it steals the last usable terminal columns. Collapse both
            // panels and leave their persistent toolbar toggles available.
            out.leftCompact = out.rightCompact = true;
        }
    } else if (showLeft || showRight) {
        // Only expose a panel when it can reach its compact readable width.
        const float compactWidth = showLeft ? leftCompactWidth : rightCompactWidth;
        const float desiredWidth = showLeft ? leftDesiredWidth : rightDesiredWidth;
        const float width = budget >= compactWidth
                                ? std::min(desiredWidth, budget)
                                : 0.0f;
        if (showLeft) {
            out.leftWidth = width;
            out.leftCompact = width < leftDesiredWidth;
        } else {
            out.rightWidth = width;
            out.rightCompact = width < rightDesiredWidth;
        }
    }
    out.centerWidth =
        std::max(0.0f, totalWidth - out.leftWidth - out.rightWidth);
    return out;
}

} // namespace liney

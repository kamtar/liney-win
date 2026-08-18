#include "app/TerminalLinks.h"

#include "util/Url.h"

#include <algorithm>

namespace liney {
namespace {

std::wstring rowText(const Grid& grid, int row, std::vector<int>& cellOfUnit) {
    std::wstring text;
    cellOfUnit.clear();
    if (row < 0 || row >= grid.rows) return text;

    for (int x = 0; x < grid.cols; ++x) {
        const Cell& cell = grid.at(x, row);
        if (cell.flags & kFlagWideTail) continue;
        if (cell.ch.empty()) {
            text.push_back(L' ');
            cellOfUnit.push_back(x);
        } else {
            for (wchar_t c : cell.ch) {
                text.push_back(c);
                cellOfUnit.push_back(x);
            }
        }
    }
    return text;
}

}  // namespace

std::vector<TerminalUrlHit> detectTerminalUrls(const Grid& grid, int row) {
    std::vector<int> cellOfUnit;
    const std::wstring text = rowText(grid, row, cellOfUnit);
    const std::vector<UrlSpan> spans = detectHttpUrls(text);
    std::vector<TerminalUrlHit> result;
    result.reserve(spans.size());

    for (const UrlSpan& span : spans) {
        if (span.start >= cellOfUnit.size() || span.end <= span.start)
            continue;
        const size_t lastUnit = std::min(span.end, cellOfUnit.size()) - 1;
        int endCell = cellOfUnit[lastUnit] + 1;
        if (endCell - 1 >= 0 && endCell - 1 < grid.cols &&
            (grid.at(endCell - 1, row).flags & kFlagWide))
            ++endCell;
        result.push_back({cellOfUnit[span.start], endCell, span.url});
    }
    return result;
}

const TerminalUrlHit* terminalUrlAt(const Grid& grid, int row, int cell,
                                    std::vector<TerminalUrlHit>& scratch) {
    scratch = detectTerminalUrls(grid, row);
    for (const TerminalUrlHit& hit : scratch)
        if (cell >= hit.startCell && cell < hit.endCell) return &hit;
    return nullptr;
}

}  // namespace liney

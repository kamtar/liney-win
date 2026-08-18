#pragma once

#include <string>
#include <vector>

#include "render/Cell.h"

namespace liney {

// A detected URL mapped onto terminal cell columns for one viewport row.
struct TerminalUrlHit {
    int startCell = 0;
    int endCell = 0;  // exclusive
    std::wstring url;
};

std::vector<TerminalUrlHit> detectTerminalUrls(const Grid& grid, int row);
const TerminalUrlHit* terminalUrlAt(const Grid& grid, int row, int cell,
                                    std::vector<TerminalUrlHit>& scratch);

}  // namespace liney

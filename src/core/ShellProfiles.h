#pragma once

#include <string>
#include <vector>

#include "core/PowerShellHistory.h"

namespace liney {

struct ShellProfile {
    std::wstring id;
    std::wstring name;
    std::wstring command;
};

// Discover supported local shells without spawning them. Results are stable,
// deduplicated, and always include cmd.exe as the final fallback.
std::vector<ShellProfile> discoverShellProfiles();

// Return the per-project/worktree PSReadLine history file. Empty paths mean
// that the session has no project/worktree identity and should keep the
// user's normal global PSReadLine history.
std::wstring powerShellHistoryPath(const std::wstring& projectPath,
                                   const std::wstring& worktreePath);

// Add Liney's OSC 7/133 bootstrap to PowerShell commands. Other shells are
// returned unchanged. When historyPath is non-empty, the bootstrap also opts
// that shell into PSReadLine HistorySavePath. The integration script is
// installed atomically in the user config directory.
std::wstring prepareShellCommand(const std::wstring& command,
                                 const std::wstring& historyPath = L"");

} // namespace liney

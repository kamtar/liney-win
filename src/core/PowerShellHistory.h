#pragma once

#include <string>

namespace liney {

// The worktree is the strongest identity. A normal project folder has no
// worktree, so it falls back to the project path. Paths are normalized for
// stable, case-insensitive Windows identities.
std::wstring powerShellHistoryIdentity(const std::wstring& projectPath,
                                       const std::wstring& worktreePath);

// Return a filesystem-safe, deterministic file name for one project/worktree.
// The caller chooses the parent directory.
std::wstring powerShellHistoryFileName(const std::wstring& projectPath,
                                       const std::wstring& worktreePath);

}  // namespace liney

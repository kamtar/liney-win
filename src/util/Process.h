#pragma once

#include <string>

namespace liney {

// Run a command and capture its stdout as UTF-8-decoded text. Runs without a
// console window (CREATE_NO_WINDOW). Returns the captured stdout (possibly
// empty); `ok` (if provided) is set to whether the process launched and exited
// with code 0. Used for `git worktree list` and similar workspace queries.
std::wstring runCapture(const std::wstring& commandLine, const std::wstring& cwd,
                        bool* ok = nullptr, unsigned long timeoutMs = 30000);

// Variant used by command-line protocols such as OpenSSH's batch-mode sftp.
// The input is UTF-8 text written to the child process stdin before capture.
std::wstring runCaptureWithInput(const std::wstring& commandLine,
                                 const std::wstring& cwd,
                                 const std::string& input,
                                 bool* ok = nullptr,
                                 unsigned long timeoutMs = 30000);

// Launch `commandLine` via `cmd /c` without waiting (fire-and-forget, no window).
// Used for lifecycle hooks (session/app exit).
void runDetached(const std::wstring& commandLine, const std::wstring& cwd);

} // namespace liney

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/SshProfiles.h"

namespace liney {

// Secrets are deliberately supplied at connection time and are never part of
// SshProfile or persisted session state. Interactive credentials are collected
// through the terminal and remain in memory only for the live SSH session.
struct SshCredentials {
    std::wstring password;
    std::wstring identityPassphrase;
    bool useAgent = true;
};

struct SshShellOptions {
    int columns = 80;
    int rows = 24;
    std::string terminalType = "xterm-256color";
};

struct SshDirectoryEntry {
    std::wstring name;
    bool isDirectory = false;
    std::uint64_t size = 0;
};

struct SshDirectoryListing {
    bool ok = false;
    std::wstring path;
    std::wstring error;
    std::vector<SshDirectoryEntry> entries;
};

using SshDirectoryRequestId = std::uint64_t;

enum class SshFileOperationKind {
    Copy,
    Move,
    Rename,
    Delete,
};

struct SshFileOperationResult {
    bool ok = false;
    std::wstring error;
};

using SshFileOperationRequestId = std::uint64_t;

struct SshConnectionCallbacks {
    // Called from the SSH worker thread. The callback must not call back into
    // SshConnection; TerminalSession uses it only to feed the VT parser and
    // wake the UI.
    std::function<void(const char* data, std::size_t length)> onOutput;
    // Called from the SSH worker when the server needs an interactive answer.
    // The terminal owns the prompt display; input is routed back through
    // writeShell() while this prompt is active. `echo` is false for passwords.
    std::function<void(const std::wstring& prompt, bool echo)> onAuthPrompt;
    // Echoes characters locally for non-secret keyboard-interactive prompts.
    std::function<void(const char* data, std::size_t length)> onAuthEcho;
    // Called from the SSH worker thread for connection/authentication errors.
    std::function<void(const std::wstring& error)> onError;
    // Called once from the SSH worker thread after the session has stopped.
    std::function<void()> onExit;
};

// Owns one TCP SSH transport. A single worker thread owns the libssh2 session,
// shell channel, and SFTP subsystem for its entire lifetime. This is required
// because libssh2 sessions must not be used concurrently from multiple
// threads. Shell input, PTY resize requests, and SFTP requests are queued so a
// blocking shell read can never hold a mutex while the FILES panel is waiting.
class SshConnection {
public:
    SshConnection();
    ~SshConnection();

    SshConnection(const SshConnection&) = delete;
    SshConnection& operator=(const SshConnection&) = delete;
    SshConnection(SshConnection&&) = delete;
    SshConnection& operator=(SshConnection&&) = delete;

    // Starts the worker and returns after the worker has been created. Network
    // connection, host-key verification, authentication, and shell setup are
    // performed off the UI thread. Invalid local arguments still fail here.
    bool start(const SshProfile& profile,
               const SshCredentials& credentials,
               const SshShellOptions& options,
               SshConnectionCallbacks callbacks,
               std::wstring* error = nullptr);

    void disconnect();

    bool isConnected() const;
    bool isAuthenticated() const;
    bool hasExited() const;

    // These methods only enqueue work and return quickly. They never call
    // libssh2 on the caller's thread.
    bool writeShell(const char* data, std::size_t length,
                    std::wstring* error = nullptr);
    bool resizeShell(int columns, int rows, std::wstring* error = nullptr);

    // Queues a typed SFTP directory listing on the same authenticated session.
    // Results are consumed with takeDirectoryResult() from the UI thread.
    SshDirectoryRequestId requestDirectory(const std::wstring& path,
                                            std::size_t maximumEntries = 500);
    std::optional<SshDirectoryListing> takeDirectoryResult(
        SshDirectoryRequestId requestId);

    // Queues a typed SFTP file operation on the same authenticated session.
    // `destination` is unused for Delete and is the full destination path for
    // Copy/Move/Rename. Results are consumed from the UI thread.
    SshFileOperationRequestId requestFileOperation(
        SshFileOperationKind kind, const std::wstring& source,
        const std::wstring& destination = L"");
    std::optional<SshFileOperationResult> takeFileOperationResult(
        SshFileOperationRequestId requestId);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace liney

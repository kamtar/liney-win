#include "core/TerminalSession.h"

#include <cwchar>
#include <cwctype>
#include <cstdlib>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <wincrypt.h>

#include "core/RenderSignal.h"
#include "core/CommandHistory.h"

namespace liney {

namespace {
std::string utf8FromWide(const wchar_t* text, size_t len) {
    if (!text || len == 0) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, text, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(len), result.data(),
                        bytes, nullptr, nullptr);
    return result;
}

bool decodeInlineImage(const SemanticEvent& event, InlineImage& image) {
    const size_t colon = event.value.find(':');
    if (colon == std::string::npos) return false;
    const std::string metadata = event.value.substr(0, colon);
    const auto hasToken = [&](const std::string& token) {
        size_t start = 0;
        while (start <= metadata.size()) {
            const size_t end = metadata.find(';', start);
            if (metadata.substr(start, end - start) == token) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    };
    if (!hasToken("inline=1")) return false;
    auto dimension = [&](const char* key, unsigned fallback) {
        const std::string needle = std::string(key) + "=";
        const size_t pos = metadata.find(needle);
        if (pos == std::string::npos) return fallback;
        unsigned value = 0;
        size_t i = pos + needle.size();
        while (i < metadata.size() && metadata[i] >= '0' &&
               metadata[i] <= '9') {
            value = value * 10 + static_cast<unsigned>(metadata[i++] - '0');
            if (value > 500) break;
        }
        return value ? std::min(value, 200u) : fallback;
    };

    const std::string encoded = event.value.substr(colon + 1);
    DWORD byteCount = 0;
    if (encoded.empty() ||
        !CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              nullptr, &byteCount, nullptr, nullptr) ||
        byteCount == 0 || byteCount > 3 * 1024 * 1024)
        return false;
    std::vector<uint8_t> bytes(byteCount);
    if (!CryptStringToBinaryA(encoded.c_str(), static_cast<DWORD>(encoded.size()),
                              CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              bytes.data(), &byteCount, nullptr, nullptr))
        return false;
    bytes.resize(byteCount);
    const bool png = bytes.size() >= 8 &&
        bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
        bytes[3] == 'G';
    const bool jpeg = bytes.size() >= 3 && bytes[0] == 0xff &&
                      bytes[1] == 0xd8 && bytes[2] == 0xff;
    const bool gif = bytes.size() >= 6 && bytes[0] == 'G' &&
                     bytes[1] == 'I' && bytes[2] == 'F';
    if (!png && !jpeg && !gif) return false;

    wchar_t tempDir[MAX_PATH]{};
    wchar_t tempPath[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempDir) ||
        !GetTempFileNameW(tempDir, L"LNY", 0, tempPath))
        return false;
    HANDLE file = CreateFileW(tempPath, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY |
                                  FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(tempPath);
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), byteCount, &written, nullptr) &&
                    written == byteCount;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(tempPath);
        return false;
    }
    image.path = tempPath;
    image.row = event.row;
    image.column = event.column;
    image.widthCells = static_cast<uint16_t>(dimension("width", 20));
    image.heightCells = static_cast<uint16_t>(dimension("height", 10));
    return true;
}

// Last path component, for a short tab/pane title.
std::wstring basename(const std::wstring& path) {
    if (path.empty()) return L"shell";
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) --end;
    size_t start = path.find_last_of(L"\\/", end ? end - 1 : 0);
    start = (start == std::wstring::npos) ? 0 : start + 1;
    std::wstring name = path.substr(start, end - start);
    return name.empty() ? path : name;
}

// CreateProcessW can block for tens of seconds while Windows resolves an
// unavailable UNC working directory (notably on Windows Server 2022). Probe
// only UNC paths off-thread and fail closed after two seconds. The detached
// probe owns both its path and shared result, so a late network response cannot
// touch a destroyed TerminalSession.
bool networkWorkingDirectoryReady(const std::wstring& path) {
    if (path.size() < 2 || path[0] != L'\\' || path[1] != L'\\') return true;
    static std::atomic<int> activeProbes{0};
    if (activeProbes.fetch_add(1) >= 4) {
        activeProbes.fetch_sub(1);
        return false;
    }
    auto result = std::make_shared<std::atomic<int>>(-1);
    std::thread([path, result]() {
        const DWORD attrs = GetFileAttributesW(path.c_str());
        *result = attrs != INVALID_FILE_ATTRIBUTES &&
                          (attrs & FILE_ATTRIBUTE_DIRECTORY)
                      ? 1 : 0;
        activeProbes.fetch_sub(1);
    }).detach();
    const ULONGLONG deadline = GetTickCount64() + 2000;
    while (result->load() < 0 && GetTickCount64() < deadline) Sleep(10);
    return result->load() == 1;
}
} // namespace

TerminalSession::~TerminalSession() {
    for (const InlineImage& image : inlineImages_)
        if (!image.path.empty()) DeleteFileW(image.path.c_str());
}

bool TerminalSession::start(const std::wstring& shell, const std::wstring& cwd,
                            int cols, int rows, int scrollback) {
    if (!networkWorkingDirectoryReady(cwd)) return false;
    ssh_.reset();
    serialProfile_.reset();
    serialTextLine_.clear();
    cwd_ = cwd;
    shell_ = shell;
    title_ = basename(cwd);
    cols_ = cols;
    rows_ = rows;

    if (!terminal_.create(cols, rows, scrollback)) return false;
    // Query responses the core emits (DSR/CPR, DA, DECRQM…) go back to the
    // child via the PTY, like a real terminal.
    terminal_.setPtyWriter(
        [this](const char* data, size_t len) { pty_.write(data, len); });
    const bool ok = pty_.start(
        shell, static_cast<short>(cols), static_cast<short>(rows), cwd,
        [this](const char* data, size_t len) {
            terminal_.write(data, len);
            markRenderDirty();  // wake the UI thread to repaint the new output
        },
        [] { markRenderDirty(); });  // exited: wake the UI so it reaps the pane
    active_ = ok;
    return ok;
}

bool TerminalSession::startSerial(const SerialProfile& profile, int cols,
                                  int rows, int scrollback) {
    if (cols <= 0 || rows <= 0) return false;
    ssh_.reset();
    cwd_.clear();
    // shellCommand() is intentionally not populated for serial sessions: it
    // is a shell restart command for existing callers. Consumers that persist
    // or reconnect serial tabs should use serialProfile().
    shell_.clear();
    title_ = serialProfileDisplayName(profile);
    cols_ = cols;
    rows_ = rows;
    rawSerialOffset_ = 0;
    serialTextLine_.clear();
    serialProfile_ = profile;
    context_.role = SessionRole::Serial;

    if (!terminal_.create(cols, rows, scrollback)) {
        serialProfile_.reset();
        return false;
    }
    terminal_.setPtyWriter(
        [this](const char* data, size_t len) { serial_.write(data, len); });
    const bool ok = serial_.start(
        profile,
        [this](const char* data, size_t len) {
            if (serialRawMode()) {
                const std::string dump =
                    formatSerialHexDump(data, len, rawSerialOffset_);
                terminal_.write(dump.data(), dump.size());
            } else if (serialRawTextMode()) {
                const std::string text = formatSerialText(data, len);
                terminal_.write(text.data(), text.size());
            } else {
                terminal_.write(data, len);
            }
            markRenderDirty();
        },
        [] { markRenderDirty(); });
    active_ = ok;
    if (!ok) serialProfile_.reset();
    return ok;
}

bool TerminalSession::startSsh(const SshProfile& profile, int cols, int rows,
                               int scrollback,
                               const SshCredentials& credentials) {
    if (!validSshProfile(profile) || cols <= 0 || rows <= 0) return false;
    serialProfile_.reset();
    serialTextLine_.clear();
    ssh_.reset();
    cwd_.clear();
    shell_ = buildSshCommand(profile);  // retained for layout persistence
    title_ = sshProfileTarget(profile);
    cols_ = cols;
    rows_ = rows;
    context_.role = SessionRole::Ssh;
    context_.sshProfile = profile;

    if (!terminal_.create(cols, rows, scrollback)) return false;
    ssh_ = std::make_unique<SshConnection>();
    terminal_.setPtyWriter(
        [this](const char* data, size_t len) { ssh_->writeShell(data, len); });

    SshShellOptions options;
    options.columns = cols;
    options.rows = rows;
    SshConnectionCallbacks callbacks;
    callbacks.onOutput = [this](const char* data, size_t len) {
        terminal_.write(data, len);
        markRenderDirty();
    };
    callbacks.onAuthPrompt = [this](const std::wstring& prompt, bool) {
        const std::string utf8 = utf8FromWide(prompt.c_str(), prompt.size());
        if (!utf8.empty()) terminal_.write(utf8.data(), utf8.size());
        markRenderDirty();
    };
    callbacks.onAuthEcho = [this](const char* data, size_t len) {
        terminal_.write(data, len);
        markRenderDirty();
    };
    callbacks.onError = [this](const std::wstring& error) {
        const std::string utf8 = utf8FromWide(error.c_str(), error.size());
        static constexpr char prefix[] = "\r\n[Liney SSH] ";
        terminal_.write(prefix, sizeof(prefix) - 1);
        if (!utf8.empty()) terminal_.write(utf8.data(), utf8.size());
        static constexpr char suffix[] = "\r\n";
        terminal_.write(suffix, sizeof(suffix) - 1);
        markRenderDirty();
    };
    callbacks.onExit = [] { markRenderDirty(); };

    std::wstring error;
    const bool ok = ssh_->start(profile, credentials, options,
                                std::move(callbacks), &error);
    if (!ok) {
        ssh_.reset();
        return false;
    }
    active_ = true;
    return true;
}

void TerminalSession::sendBytes(const char* data, size_t len) {
    if (active_) {
        if (!isSerial()) capturePromptInput(data, len);
        if (isSsh())
            ssh_->writeShell(data, len);
        else if (isSerial())
            serial_.write(data, len);
        else
            pty_.write(data, len);
    }
}

SshDirectoryRequestId TerminalSession::requestSftpDirectory(
    const std::wstring& path, std::size_t maximumEntries) {
    return isSsh() ? ssh_->requestDirectory(path, maximumEntries) : 0;
}

std::optional<SshDirectoryListing> TerminalSession::takeSftpDirectoryResult(
    SshDirectoryRequestId requestId) {
    return isSsh() ? ssh_->takeDirectoryResult(requestId) : std::nullopt;
}

SshFileOperationRequestId TerminalSession::requestSftpFileOperation(
    SshFileOperationKind kind, const std::wstring& source,
    const std::wstring& destination) {
    return isSsh() ? ssh_->requestFileOperation(kind, source, destination) : 0;
}

std::optional<SshFileOperationResult>
TerminalSession::takeSftpFileOperationResult(
    SshFileOperationRequestId requestId) {
    return isSsh() ? ssh_->takeFileOperationResult(requestId) : std::nullopt;
}

bool TerminalSession::sendSerialHexInput(const std::wstring& input,
                                         std::wstring* error) {
    if (!isSerial()) {
        if (error) *error = L"The active session is not serial.";
        return false;
    }
    if (!active_ || serial_.hasExited()) {
        if (error) *error = L"The serial device is not connected.";
        return false;
    }
    std::string bytes;
    if (!parseSerialHexInput(input, bytes, error)) return false;
    sendBytes(bytes.data(), bytes.size());
    return true;
}

void TerminalSession::appendSerialText(const wchar_t* text, size_t len) {
    if (!serialRawTextMode() || !active_ || serial_.hasExited() ||
        !text || len == 0 || serialTextLine_.size() >= 64 * 1024)
        return;
    const size_t room = 64 * 1024 - serialTextLine_.size();
    const size_t accepted = std::min(len, room);
    const std::string utf8 = utf8FromWide(text, accepted);
    if (utf8.empty()) return;
    serialTextLine_.append(text, accepted);
    terminal_.write(utf8.data(), utf8.size());
    markRenderDirty();
}

void TerminalSession::backspaceSerialText() {
    if (!serialRawTextMode() || serialTextLine_.empty()) return;
    size_t remove = 1;
    const wchar_t last = serialTextLine_.back();
    if (last >= 0xDC00 && last <= 0xDFFF && serialTextLine_.size() >= 2) {
        const wchar_t previous = serialTextLine_[serialTextLine_.size() - 2];
        if (previous >= 0xD800 && previous <= 0xDBFF) remove = 2;
    }
    serialTextLine_.resize(serialTextLine_.size() - remove);
    static constexpr char kErase[] = "\b \b";
    terminal_.write(kErase, sizeof(kErase) - 1);
    markRenderDirty();
}

void TerminalSession::clearSerialText() {
    while (serialRawTextMode() && !serialTextLine_.empty())
        backspaceSerialText();
}

void TerminalSession::submitSerialText() {
    if (!serialRawTextMode() || !active_ || serial_.hasExited()) return;
    std::string payload = utf8FromWide(serialTextLine_.c_str(),
                                       serialTextLine_.size());
    switch (serialProfile_->lineEnding) {
    case SerialLineEnding::CarriageReturn: payload += '\r'; break;
    case SerialLineEnding::LineFeed: payload += '\n'; break;
    case SerialLineEnding::CarriageReturnLineFeed: payload += "\r\n"; break;
    case SerialLineEnding::None: break;
    }
    serial_.write(payload.data(), payload.size());
    static constexpr char kNewline[] = "\r\n";
    terminal_.write(kNewline, sizeof(kNewline) - 1);
    serialTextLine_.clear();
    markRenderDirty();
}

void TerminalSession::capturePromptInput(const char* data, size_t len) {
    if (!atPrompt_) return;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(data[i]);
        if (promptEscape_) {
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                ch == '~') promptEscape_ = false;
            continue;
        }
        if (ch == 0x1b) { promptEscape_ = true; continue; }
        if (ch == 0x08 || ch == 0x7f) {
            if (!promptInputUtf8_.empty()) promptInputUtf8_.pop_back();
        } else if (ch == '\r' || ch == '\n') {
            // Keep the accepted line until OSC 133;C starts command output.
            pendingCommandStartedAt_ = std::chrono::steady_clock::now();
        } else if (ch >= 0x20 && ch != 0x7f) {
            if (promptInputUtf8_.size() < 64 * 1024)
                promptInputUtf8_.push_back(static_cast<char>(ch));
        }
    }
}

void TerminalSession::processSemanticEvents() {
    auto startBlock = [&]() {
        if (!commandBlocks_.empty() &&
            commandBlocks_.back().state == CommandState::Running) return;
        CommandBlock block;
        block.id = nextCommandId_++;
        block.cwd = cwd_;
        block.startedAt = pendingCommandStartedAt_.time_since_epoch().count() != 0
                              ? pendingCommandStartedAt_
                              : std::chrono::steady_clock::now();
        block.startRow = pendingCommandRow_;
        if (!promptInputUtf8_.empty()) {
            const int count = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, promptInputUtf8_.data(),
                static_cast<int>(promptInputUtf8_.size()), nullptr, 0);
            if (count > 0) {
                block.command.resize(static_cast<size_t>(count));
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    promptInputUtf8_.data(),
                                    static_cast<int>(promptInputUtf8_.size()),
                                    block.command.data(), count);
            }
        }
        commandBlocks_.push_back(std::move(block));
        if (commandBlocks_.size() > 1000) commandBlocks_.erase(commandBlocks_.begin());
        promptInputUtf8_.clear();
        pendingCommandStartedAt_ = {};
    };
    for (auto& event : terminal_.drainSemanticEvents()) {
        switch (event.type) {
        case SemanticEventType::PromptStart:
            atPrompt_ = false;
            break;
        case SemanticEventType::CommandStart:
            atPrompt_ = true;
            pendingCommandRow_ = event.row;
            promptEscape_ = false;
            pendingCommandStartedAt_ = {};
            promptInputUtf8_.clear();
            break;
        case SemanticEventType::OutputStart:
            atPrompt_ = false;
            startBlock();
            break;
        case SemanticEventType::CommandEnd:
            // Shells with partial integration may omit C. Still retain the
            // command and complete it when the next prompt emits D.
            if (!promptInputUtf8_.empty()) startBlock();
            if (!commandBlocks_.empty() &&
                commandBlocks_.back().state == CommandState::Running) {
                CommandBlock& block = commandBlocks_.back();
                wchar_t* end = nullptr;
                const std::wstring code(event.value.begin(), event.value.end());
                const long parsed = event.value.empty() ? 0 : wcstol(code.c_str(), &end, 10);
                block.exitCode = event.value.empty()
                                     ? 0
                                     : (end && *end == L'\0'
                                            ? static_cast<int>(parsed)
                                            : -1);
                block.state = block.exitCode == 0 ? CommandState::Succeeded
                                                  : CommandState::Failed;
                block.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - block.startedAt);
                block.endRow = event.row;
                commandNavigation_ = commandBlocks_.size();
                FILETIME ft{}; GetSystemTimeAsFileTime(&ft);
                ULARGE_INTEGER ticks{}; ticks.HighPart = ft.dwHighDateTime;
                ticks.LowPart = ft.dwLowDateTime;
                appendCommandHistory({block.command, block.cwd, block.exitCode,
                                      ticks.QuadPart});
            }
            break;
        case SemanticEventType::ClipboardRequest:
            // UI must explicitly approve before decoding/writing the clipboard.
            clipboardRequest_ = std::move(event.value);
            break;
        case SemanticEventType::HyperlinkStart:
        case SemanticEventType::HyperlinkEnd:
            break;
        case SemanticEventType::AgentStatus:
            if (event.value == "running") reportedAgentActivity_ = AgentActivity::Running;
            else if (event.value == "waiting") reportedAgentActivity_ = AgentActivity::Waiting;
            else if (event.value == "needs-input") reportedAgentActivity_ = AgentActivity::NeedsInput;
            else if (event.value == "done") reportedAgentActivity_ = AgentActivity::Done;
            else if (event.value == "failed") reportedAgentActivity_ = AgentActivity::Failed;
            break;
        case SemanticEventType::InlineImage: {
            InlineImage image;
            if (decodeInlineImage(event, image)) {
                if (inlineImages_.size() >= 16) {
                    DeleteFileW(inlineImages_.front().path.c_str());
                    inlineImages_.erase(inlineImages_.begin());
                }
                inlineImages_.push_back(std::move(image));
            }
            break;
        }
        }
    }
}

AgentActivity TerminalSession::agentActivity() const {
    if (context_.role != SessionRole::Agent) return AgentActivity::Idle;
    if (!pty_.hasExited()) {
        return reportedAgentActivity_ == AgentActivity::Waiting ||
                       reportedAgentActivity_ == AgentActivity::NeedsInput
                   ? reportedAgentActivity_
                   : AgentActivity::Running;
    }
    unsigned long code = 0;
    if (pty_.exitCode(code)) return code == 0 ? AgentActivity::Done
                                              : AgentActivity::Failed;
    return reportedAgentActivity_ == AgentActivity::Failed
               ? AgentActivity::Failed
               : AgentActivity::Done;
}

std::string TerminalSession::takeClipboardRequest() {
    std::string out;
    out.swap(clipboardRequest_);
    return out;
}

std::string TerminalSession::commandOutputUtf8(size_t index) {
    if (index >= commandBlocks_.size()) return {};
    std::string buffer;
    if (!terminal_.dumpBufferUtf8(buffer)) return {};
    const CommandBlock& block = commandBlocks_[index];
    std::string output;
    uint64_t row = 0;
    size_t pos = 0;
    while (pos <= buffer.size()) {
        size_t end = buffer.find('\n', pos);
        if (end == std::string::npos) end = buffer.size();
        if (row >= block.startRow && row <= block.endRow) {
            output.append(buffer, pos, end - pos);
            output.push_back('\n');
        }
        if (end == buffer.size()) break;
        pos = end + 1;
        ++row;
    }
    return output;
}

bool TerminalSession::jumpPreviousCommand() {
    if (commandBlocks_.empty()) return false;
    if (commandNavigation_ == 0 || commandNavigation_ > commandBlocks_.size())
        commandNavigation_ = commandBlocks_.size();
    --commandNavigation_;
    terminal_.scrollToRow(commandBlocks_[commandNavigation_].startRow);
    return true;
}

bool TerminalSession::jumpNextCommand() {
    if (commandBlocks_.empty()) return false;
    if (commandNavigation_ + 1 >= commandBlocks_.size()) {
        commandNavigation_ = commandBlocks_.size();
        terminal_.scrollToBottom();
        return true;
    }
    ++commandNavigation_;
    terminal_.scrollToRow(commandBlocks_[commandNavigation_].startRow);
    return true;
}

void TerminalSession::toggleBookmarkLastCommand() {
    if (!commandBlocks_.empty())
        commandBlocks_.back().bookmarked = !commandBlocks_.back().bookmarked;
}

void TerminalSession::resize(int cols, int rows, int cellWidthPx,
                             int cellHeightPx) {
    if (!active_ || cols <= 0 || rows <= 0) return;
    // Same grid but a new cell pixel size (font/DPI change) still needs to
    // reach the core: pixel metrics feed mouse reporting and size reports.
    if (cols == cols_ && rows == rows_ && cellWidthPx == cellW_ &&
        cellHeightPx == cellH_)
        return;
    cols_ = cols;
    rows_ = rows;
    cellW_ = cellWidthPx;
    cellH_ = cellHeightPx;
    terminal_.resize(cols, rows, cellWidthPx, cellHeightPx);
    if (isSsh())
        ssh_->resizeShell(cols, rows);
    else if (!isSerial())
        pty_.resize(static_cast<short>(cols), static_cast<short>(rows));
}

void TerminalSession::snapshot() {
    if (active_) terminal_.snapshotInto(grid_);
}

std::wstring TerminalSession::prettifyTitle(const std::wstring& t) const {
    // Lower-cased copy for case-insensitive matching (titles come from the
    // console host with whatever casing the shell used).
    std::wstring low = t;
    for (wchar_t& ch : low) ch = static_cast<wchar_t>(towlower(ch));

    // Does the title start with a path to a known shell executable?
    static const wchar_t* kShells[] = { L"cmd.exe", L"powershell.exe",
                                        L"pwsh.exe", L"wsl.exe" };
    for (const wchar_t* exe : kShells) {
        const size_t pos = low.find(exe);
        if (pos == std::wstring::npos) continue;
        // Only treat it as "the shell's own path" when the exe name ends the
        // path token (start of string or after a separator, and followed by
        // end / " - <command>").
        const size_t end = pos + wcslen(exe);
        const bool pathLike =
            pos == 0 || low[pos - 1] == L'\\' || low[pos - 1] == L'/';
        if (!pathLike) continue;
        if (end == low.size()) {
            // Bare shell path (idle prompt): show the directory name instead.
            return basename(cwd_.empty() ? shell_ : cwd_);
        }
        // "…cmd.exe - <command>": show just the command.
        const std::wstring rest = t.substr(end);
        const size_t dash = rest.find(L" - ");
        if (dash != std::wstring::npos) {
            std::wstring cmd = rest.substr(dash + 3);
            // Trim leading spaces (cmd double-spaces some titles).
            size_t s = cmd.find_first_not_of(L' ');
            if (s != std::wstring::npos) return cmd.substr(s);
        }
    }
    return t;  // an app-provided title (vim, ssh, …): keep as-is
}

} // namespace liney

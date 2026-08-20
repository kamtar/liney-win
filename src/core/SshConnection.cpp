#include "core/SshConnection.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace liney {
namespace {

std::string wideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            bytes, nullptr, nullptr) != bytes)
        return {};
    return result;
}

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int chars = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (chars <= 0) return {};
    std::wstring result(static_cast<size_t>(chars), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            chars) != chars)
        return {};
    return result;
}

std::wstring win32Error(const wchar_t* operation, DWORD code) {
    wchar_t message[512]{};
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, message, static_cast<DWORD>(_countof(message)),
        nullptr);
    std::wstring result = operation;
    result += L" failed";
    if (length != 0) {
        result += L": ";
        result.append(message, length);
        while (!result.empty() &&
               (result.back() == L'\r' || result.back() == L'\n'))
            result.pop_back();
    }
    return result;
}

std::wstring currentWindowsUser() {
    wchar_t buffer[256]{};
    DWORD length = static_cast<DWORD>(_countof(buffer));
    if (GetUserNameW(buffer, &length) && length > 0) {
        if (buffer[length - 1] == L'\0') --length;
        return std::wstring(buffer, length);
    }
    return {};
}

std::wstring knownHostsPath() {
    wchar_t profile[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(
        L"USERPROFILE", profile, static_cast<DWORD>(_countof(profile)));
    if (length == 0 || length >= _countof(profile)) return {};
    return std::wstring(profile, length) + L"\\.ssh\\known_hosts";
}

std::wstring sshError(LIBSSH2_SESSION* session, const wchar_t* fallback) {
    char* message = nullptr;
    int length = 0;
    const int code = libssh2_session_last_error(
        session, &message, &length, 0);
    if (message && length > 0) {
        const std::wstring converted =
            utf8ToWide(std::string_view(message, static_cast<size_t>(length)));
        if (!converted.empty())
            return std::wstring(fallback) + L": " + converted;
    }
    return std::wstring(fallback) + L" (libssh2 error " +
           std::to_wstring(code) + L")";
}

int knownHostKeyType(int type) {
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

bool initializeNetworkAndLibssh2(std::wstring* error) {
    static std::once_flag once;
    static int networkResult = 0;
    static int libsshResult = 0;
    std::call_once(once, [] {
        WSADATA data{};
        networkResult = WSAStartup(MAKEWORD(2, 2), &data);
        libsshResult = networkResult == 0 ? libssh2_init(0) : -1;
    });
    if (networkResult != 0) {
        if (error) *error = win32Error(L"WSAStartup", networkResult);
        return false;
    }
    if (libsshResult != 0) {
        if (error) *error = L"libssh2 initialization failed (" +
                              std::to_wstring(libsshResult) + L")";
        return false;
    }
    return true;
}

}  // namespace

struct SshConnection::Impl {
    struct DirectoryRequest {
        SshDirectoryRequestId id = 0;
        std::wstring path;
        std::size_t maximumEntries = 500;
    };

    struct DirectoryJob {
        DirectoryRequest request;
        SshDirectoryListing result;
        LIBSSH2_SFTP_HANDLE* handle = nullptr;
        LIBSSH2_CHANNEL* rootChannel = nullptr;
        bool rootAttempted = false;
        bool rootActive = false;
        bool closePending = false;
        std::string rootOutput;
        std::string rootInput;
        std::size_t rootInputOffset = 0;
        char name[4096]{};
    };

    struct FileOperationRequest {
        SshFileOperationRequestId id = 0;
        SshFileOperationKind kind = SshFileOperationKind::Copy;
        std::wstring source;
        std::wstring destination;
    };

    SshProfile profile;
    SshCredentials credentials;
    SshShellOptions shellOptions;
    SshConnectionCallbacks callbacks;

    SOCKET socket = INVALID_SOCKET;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_CHANNEL* shell = nullptr;
    LIBSSH2_SFTP* sftp = nullptr;

    std::thread worker;
    mutable std::mutex queueMutex;
    std::condition_variable queueCv;
    std::string inputQueue;
    std::optional<std::pair<int, int>> pendingResize;
    std::deque<DirectoryRequest> directoryQueue;
    std::deque<FileOperationRequest> fileOperationQueue;
    std::mutex resultMutex;
    std::unordered_map<SshDirectoryRequestId, SshDirectoryListing> results;
    std::mutex fileOperationResultMutex;
    std::unordered_map<SshFileOperationRequestId, SshFileOperationResult>
        fileOperationResults;
    SshDirectoryRequestId nextRequestId = 1;
    std::optional<DirectoryJob> directoryJob;

    std::mutex authMutex;
    std::condition_variable authCv;
    bool authPromptActive = false;
    bool authResponseReady = false;
    bool authCancelled = false;
    bool authEcho = false;
    std::string authInput;
    std::string authResponse;
    std::string sudoPassword;
    std::string shellTail;
    bool rootShellActive = false;

    std::atomic<bool> started{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> authenticated{false};
    std::atomic<bool> exited{true};

    ~Impl() { stop(); }

    void stop() {
        stopRequested.store(true, std::memory_order_release);
        queueCv.notify_all();
        authCv.notify_all();
        if (worker.joinable()) worker.join();
        started.store(false, std::memory_order_release);
    }

    bool submitAuthInput(const char* data, std::size_t length) {
        if (!data || length == 0) return false;
        std::string echoed;
        bool handled = false;
        {
            std::lock_guard lock(authMutex);
            if (!authPromptActive) return false;
            handled = true;
            for (std::size_t i = 0; i < length; ++i) {
                const unsigned char ch = static_cast<unsigned char>(data[i]);
                if (ch == 0x03 || ch == 0x1b) {
                    authCancelled = true;
                    authResponseReady = true;
                    break;
                }
                if (ch == '\b' || ch == 0x7f) {
                    if (!authInput.empty()) {
                        authInput.pop_back();
                        if (authEcho) echoed += "\b \b";
                    }
                    continue;
                }
                if (ch == '\r' || ch == '\n') {
                    authResponse = authInput;
                    authResponseReady = true;
                    // Password characters stay hidden, but the submitted
                    // answer still advances the terminal to the next line.
                    echoed += "\r\n";
                    break;
                }
                if (ch >= 0x20 && ch != 0x7f) {
                    authInput.push_back(static_cast<char>(ch));
                    if (authEcho) echoed.push_back(static_cast<char>(ch));
                }
            }
        }
        if (!echoed.empty() && callbacks.onAuthEcho)
            callbacks.onAuthEcho(echoed.data(), echoed.size());
        if (handled) authCv.notify_one();
        return handled;
    }

    std::string requestAuthResponse(const std::wstring& prompt, bool echo) {
        {
            std::lock_guard lock(authMutex);
            authPromptActive = true;
            authResponseReady = false;
            authCancelled = false;
            authEcho = echo;
            authInput.clear();
            authResponse.clear();
        }
        if (callbacks.onAuthPrompt) callbacks.onAuthPrompt(prompt, echo);
        std::unique_lock lock(authMutex);
        authCv.wait(lock, [this] {
            return authResponseReady ||
                   stopRequested.load(std::memory_order_acquire);
        });
        const bool cancelled = authCancelled ||
            stopRequested.load(std::memory_order_acquire);
        std::string response = cancelled ? std::string{} : authResponse;
        if (!authResponseReady && !authInput.empty()) response = authInput;
        if (!response.empty() && prompt.find(L"assword") != std::wstring::npos)
            sudoPassword = response;
        if (!authInput.empty()) {
            SecureZeroMemory(authInput.data(), authInput.size());
            authInput.clear();
        }
        if (!authResponse.empty()) {
            SecureZeroMemory(authResponse.data(), authResponse.size());
            authResponse.clear();
        }
        authPromptActive = false;
        authResponseReady = false;
        authCancelled = false;
        authEcho = false;
        lock.unlock();
        authCv.notify_all();
        return response;
    }

    static void keyboardInteractiveCallback(
        const char*, int, const char*, int, int numPrompts,
        const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
        LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
        auto* impl = abstract && *abstract
            ? static_cast<Impl*>(*abstract) : nullptr;
        if (!impl || !prompts || !responses || numPrompts < 0) return;
        for (int i = 0; i < numPrompts; ++i) {
            const std::string promptUtf8(
                reinterpret_cast<const char*>(prompts[i].text),
                prompts[i].length);
            const std::wstring prompt = utf8ToWide(promptUtf8);
            std::string response = impl->requestAuthResponse(
                prompt.empty() ? L"SSH response: " : prompt,
                prompts[i].echo != 0);
            if (!response.empty()) {
                responses[i].text = static_cast<char*>(
                    std::malloc(response.size() + 1));
                if (responses[i].text) {
                    std::memcpy(responses[i].text, response.data(),
                                response.size());
                    responses[i].text[response.size()] = '\0';
                    responses[i].length = static_cast<unsigned int>(
                        response.size());
                }
            }
            if (!response.empty()) {
                SecureZeroMemory(response.data(), response.size());
                response.clear();
            }
        }
    }

    void setError(std::wstring* error, std::wstring value) {
        if (error) *error = std::move(value);
    }

    bool waitForIo(unsigned long timeoutMs = 100, bool allowStop = false) {
        if (!allowStop && stopRequested.load(std::memory_order_acquire))
            return false;
        fd_set readSet{};
        fd_set writeSet{};
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        bool wantRead = true;
        bool wantWrite = true;
        if (session) {
            const int directions = libssh2_session_block_directions(session);
            wantRead = (directions & LIBSSH2_SESSION_BLOCK_INBOUND) != 0;
            wantWrite = (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0;
            if (!wantRead && !wantWrite) wantRead = wantWrite = true;
        }
        if (wantRead) FD_SET(socket, &readSet);
        if (wantWrite) FD_SET(socket, &writeSet);
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(timeoutMs / 1000);
        timeout.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        const int result = select(0, &readSet, &writeSet, nullptr, &timeout);
        return result >= 0 &&
               (allowStop || !stopRequested.load(std::memory_order_acquire));
    }

    template <typename Function>
    int retryInt(Function&& function, unsigned long timeoutMs = 15000,
                 bool allowStop = false) {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            const int result = function();
            if (result != LIBSSH2_ERROR_EAGAIN) return result;
            if (GetTickCount64() >= deadline || !waitForIo(100, allowStop))
                return LIBSSH2_ERROR_TIMEOUT;
        }
    }

    template <typename Function>
    auto retryPointer(Function&& function, unsigned long timeoutMs = 15000)
        -> decltype(function()) {
        using Pointer = decltype(function());
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        for (;;) {
            Pointer result = function();
            if (result != nullptr) return result;
            if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN)
                return nullptr;
            if (GetTickCount64() >= deadline || !waitForIo(100)) return nullptr;
        }
    }

    void completeDirectory(SshDirectoryListing result,
                           SshDirectoryRequestId id) {
        std::lock_guard lock(resultMutex);
        results[id] = std::move(result);
    }

    void completeFileOperation(SshFileOperationResult result,
                               SshFileOperationRequestId id) {
        std::lock_guard lock(fileOperationResultMutex);
        fileOperationResults[id] = std::move(result);
    }

    void failQueuedFileOperations(const std::wstring& error) {
        std::deque<FileOperationRequest> queued;
        {
            std::lock_guard lock(queueMutex);
            queued.swap(fileOperationQueue);
        }
        for (const FileOperationRequest& request : queued)
            completeFileOperation({false, error}, request.id);
    }

    static std::string shellQuote(const std::string& value) {
        std::string quoted = "'";
        for (const char ch : value) {
            if (ch == '\'') quoted += "'\\''";
            else quoted.push_back(ch);
        }
        quoted.push_back('\'');
        return quoted;
    }

    static std::string rootDirectoryCommand(const std::string& path,
                                             bool withPassword) {
        // `sudo -n` deliberately never opens a password prompt on this
        // auxiliary channel. After `sudo su`, the user's sudo timestamp lets
        // this command run as root; otherwise the normal SFTP fallback is
        // used. The path is passed as $1, never interpolated into the script.
        const char* sudo = withPassword ? "sudo -S -p '' -- "
                                        : "sudo -n -- ";
        return std::string(sudo) +
               "/bin/sh -c 'LC_ALL=C /usr/bin/find -- \"$1\" "
               "-mindepth 1 -maxdepth 1 -printf \"%f\\t%y\\t%s\\n\"' "
               "liney-sftp " + shellQuote(path);
    }

    static void closeRootChannel(DirectoryJob& job) {
        if (job.rootChannel) {
            libssh2_channel_close(job.rootChannel);
            libssh2_channel_free(job.rootChannel);
            job.rootChannel = nullptr;
        }
        if (!job.rootInput.empty()) {
            SecureZeroMemory(job.rootInput.data(), job.rootInput.size());
            job.rootInput.clear();
        }
        job.rootInputOffset = 0;
        job.rootActive = false;
    }

    static bool parseRootListing(DirectoryJob& job) {
        std::size_t start = 0;
        while (start < job.rootOutput.size() &&
               job.result.entries.size() < job.request.maximumEntries) {
            const std::size_t end = job.rootOutput.find('\n', start);
            const std::size_t lineEnd = end == std::string::npos
                ? job.rootOutput.size() : end;
            std::string line = job.rootOutput.substr(start, lineEnd - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t first = line.find('\t');
            const std::size_t second = first == std::string::npos
                ? std::string::npos : line.find('\t', first + 1);
            if (first != std::string::npos && second != std::string::npos &&
                first > 0 && second > first + 1) {
                SshDirectoryEntry entry;
                entry.name = utf8ToWide(std::string_view(line.data(), first));
                const std::string type = line.substr(first + 1, second - first - 1);
                const std::string size = line.substr(second + 1);
                if (!entry.name.empty()) {
                    entry.isDirectory = type == "d";
                    try {
                        entry.size = static_cast<std::uint64_t>(
                            std::stoull(size));
                    } catch (...) {
                        entry.size = 0;
                    }
                    job.result.entries.push_back(std::move(entry));
                }
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return true;
    }

    void failQueuedDirectories(const std::wstring& error) {
        std::deque<DirectoryRequest> queued;
        {
            std::lock_guard lock(queueMutex);
            queued.swap(directoryQueue);
        }
        for (const DirectoryRequest& request : queued) {
            SshDirectoryListing result;
            result.path = request.path.empty() ? L"." : request.path;
            result.error = error;
            completeDirectory(std::move(result), request.id);
        }
        if (directoryJob) {
            if (directoryJob->handle) {
                retryInt([&] {
                    return libssh2_sftp_closedir(directoryJob->handle);
                }, 2000, true);
                directoryJob->handle = nullptr;
            }
        closeRootChannel(*directoryJob);
        directoryJob->result.error = error;
            completeDirectory(std::move(directoryJob->result),
                              directoryJob->request.id);
            directoryJob.reset();
        }
    }

    void closeResources() {
        if (directoryJob) {
            if (directoryJob->handle) {
                retryInt([&] {
                    return libssh2_sftp_closedir(directoryJob->handle);
                }, 2000, true);
                directoryJob->handle = nullptr;
            }
            closeRootChannel(*directoryJob);
        }
        if (shell) {
            libssh2_channel_close(shell);
            libssh2_channel_free(shell);
            shell = nullptr;
        }
        if (sftp) {
            libssh2_sftp_shutdown(sftp);
            sftp = nullptr;
        }
        if (session) {
            libssh2_session_disconnect(session, "Liney closing SSH session");
            libssh2_session_free(session);
            session = nullptr;
        }
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        connected.store(false, std::memory_order_release);
        authenticated.store(false, std::memory_order_release);
        if (!credentials.password.empty()) {
            SecureZeroMemory(credentials.password.data(),
                             credentials.password.size() * sizeof(wchar_t));
            credentials.password.clear();
        }
        if (!credentials.identityPassphrase.empty()) {
            SecureZeroMemory(credentials.identityPassphrase.data(),
                             credentials.identityPassphrase.size() * sizeof(wchar_t));
            credentials.identityPassphrase.clear();
        }
        if (!sudoPassword.empty()) {
            SecureZeroMemory(sudoPassword.data(), sudoPassword.size());
            sudoPassword.clear();
        }
    }

    bool connectSocket(std::wstring* error) {
        const std::string host = wideToUtf8(profile.host);
        if (host.empty()) {
            setError(error, L"SSH host is not valid UTF-8");
            return false;
        }
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        addrinfo* addresses = nullptr;
        const std::string port = std::to_string(profile.port);
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &addresses) != 0) {
            setError(error, L"SSH address lookup failed");
            return false;
        }
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            if (stopRequested.load(std::memory_order_acquire)) break;
            SOCKET candidate = ::socket(address->ai_family, address->ai_socktype,
                                        address->ai_protocol);
            if (candidate == INVALID_SOCKET) continue;
            u_long nonBlocking = 1;
            ioctlsocket(candidate, FIONBIO, &nonBlocking);
            const int rc = ::connect(candidate, address->ai_addr,
                                     static_cast<int>(address->ai_addrlen));
            const int connectError = rc == 0 ? 0 : WSAGetLastError();
            if (rc != 0 && connectError != WSAEWOULDBLOCK &&
                connectError != WSAEINPROGRESS) {
                closesocket(candidate);
                continue;
            }
            int ready = rc == 0 ? 1 : 0;
            const ULONGLONG deadline = GetTickCount64() + 10000;
            while (ready == 0 && !stopRequested.load(std::memory_order_acquire) &&
                   GetTickCount64() < deadline) {
                fd_set writeSet{};
                FD_ZERO(&writeSet);
                FD_SET(candidate, &writeSet);
                timeval timeout{0, 100000};
                ready = select(0, nullptr, &writeSet, nullptr, &timeout);
                if (ready < 0) break;
            }
            int socketError = 0;
            int socketErrorSize = sizeof(socketError);
            getsockopt(candidate, SOL_SOCKET, SO_ERROR,
                       reinterpret_cast<char*>(&socketError), &socketErrorSize);
            if (!stopRequested.load(std::memory_order_acquire) &&
                (rc == 0 || ready > 0) && socketError == 0) {
                socket = candidate;
                break;
            }
            closesocket(candidate);
        }
        freeaddrinfo(addresses);
        if (socket == INVALID_SOCKET) {
            if (!stopRequested.load(std::memory_order_acquire))
                setError(error, L"SSH socket connection timed out or failed");
            return false;
        }
        return true;
    }

    bool verifyHostKey(std::wstring* error) {
        size_t keyLength = 0;
        int keyType = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
        const char* key = libssh2_session_hostkey(session, &keyLength, &keyType);
        const std::string host = wideToUtf8(profile.host);
        const std::string file = wideToUtf8(knownHostsPath());
        if (!key || keyLength == 0 || host.empty() || file.empty()) {
            setError(error, L"Could not verify the SSH host key");
            return false;
        }
        LIBSSH2_KNOWNHOSTS* knownHosts = libssh2_knownhost_init(session);
        if (!knownHosts) {
            setError(error, sshError(session, L"Could not initialize known_hosts"));
            return false;
        }
        const int loaded = libssh2_knownhost_readfile(
            knownHosts, file.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        if (loaded < 0) {
            libssh2_knownhost_free(knownHosts);
            setError(error, L"Could not read SSH known_hosts: " + knownHostsPath());
            return false;
        }
        const int check = libssh2_knownhost_checkp(
            knownHosts, host.c_str(), profile.port, key, keyLength,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                knownHostKeyType(keyType), nullptr);
        libssh2_knownhost_free(knownHosts);
        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) return true;
        setError(error, check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH
                           ? L"SSH host key does not match known_hosts"
                           : L"SSH host key is not present in known_hosts");
        return false;
    }

    bool authenticateWithAgent(const std::string& username) {
        LIBSSH2_AGENT* agent = libssh2_agent_init(session);
        if (!agent) return false;
        bool authenticatedWithAgent = false;
        if (libssh2_agent_connect(agent) == 0 &&
            libssh2_agent_list_identities(agent) == 0) {
            struct libssh2_agent_publickey* identity = nullptr;
            while (libssh2_agent_get_identity(agent, &identity, identity) == 0) {
                if (retryInt([&] {
                        return libssh2_agent_userauth(
                            agent, username.c_str(), identity);
                    }) == 0) {
                    authenticatedWithAgent = true;
                    break;
                }
            }
        }
        libssh2_agent_disconnect(agent);
        libssh2_agent_free(agent);
        return authenticatedWithAgent;
    }

    bool establish(std::wstring* error) {
        if (!initializeNetworkAndLibssh2(error) || !connectSocket(error)) return false;
        sudoPassword = wideToUtf8(credentials.password);
        session = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
        if (!session) {
            setError(error, L"Could not allocate the libssh2 session");
            return false;
        }
        libssh2_session_set_blocking(session, 0);
        if (retryInt([this] { return libssh2_session_handshake(session, socket); }) != 0) {
            setError(error, sshError(session, L"SSH handshake failed"));
            return false;
        }
        if (!verifyHostKey(error)) return false;

        const std::string username = wideToUtf8(
            profile.user.empty() ? currentWindowsUser() : profile.user);
        if (username.empty()) {
            setError(error, L"SSH user is not valid UTF-8");
            return false;
        }
        bool authenticatedNow = false;
        const std::string identity = wideToUtf8(profile.identityFile);
        const std::string passphrase = wideToUtf8(credentials.identityPassphrase);
        if (!identity.empty()) {
            const int keyResult = retryInt([&] {
                return libssh2_userauth_publickey_fromfile(
                    session, username.c_str(), nullptr, identity.c_str(),
                    passphrase.empty() ? nullptr : passphrase.c_str());
            });
            authenticatedNow = keyResult == 0;
            if (!authenticatedNow && passphrase.empty() &&
                keyResult == LIBSSH2_ERROR_KEYFILE_AUTH_FAILED) {
                std::string entered = requestAuthResponse(
                    L"Passphrase for SSH key (leave blank if unencrypted): ",
                    false);
                authenticatedNow = retryInt([&] {
                    return libssh2_userauth_publickey_fromfile(
                        session, username.c_str(), nullptr, identity.c_str(),
                        entered.empty() ? nullptr : entered.c_str());
                }) == 0;
                if (!entered.empty()) {
                    SecureZeroMemory(entered.data(), entered.size());
                    entered.clear();
                }
            }
        }
        if (!authenticatedNow && credentials.useAgent)
            authenticatedNow = authenticateWithAgent(username);
        const std::string password = wideToUtf8(credentials.password);
        if (!authenticatedNow && !password.empty()) {
            authenticatedNow = retryInt([&] {
                return libssh2_userauth_password(
                    session, username.c_str(), password.c_str());
            }) == 0;
        }
        if (!authenticatedNow) {
            char* methods = retryPointer([this, &username] {
                return libssh2_userauth_list(
                    session, username.c_str(),
                    static_cast<unsigned int>(username.size()));
            });
            const std::string available = methods ? methods : "";
            if (available.find("keyboard-interactive") != std::string::npos) {
                authenticatedNow = retryInt([&] {
                    return libssh2_userauth_keyboard_interactive(
                        session, username.c_str(),
                        &Impl::keyboardInteractiveCallback);
                }) == 0;
            } else if (available.find("password") != std::string::npos) {
                std::string entered = requestAuthResponse(
                    L"Password: ", false);
                if (!entered.empty()) sudoPassword = entered;
                authenticatedNow = retryInt([&] {
                    return libssh2_userauth_password(
                        session, username.c_str(), entered.c_str());
                }) == 0;
                if (!entered.empty()) {
                    SecureZeroMemory(entered.data(), entered.size());
                    entered.clear();
                }
            }
        }
        if (!authenticatedNow) {
            setError(error, sshError(session, L"SSH authentication failed"));
            return false;
        }
        authenticated.store(true, std::memory_order_release);

        shell = retryPointer([this] { return libssh2_channel_open_session(session); });
        if (!shell) {
            setError(error, sshError(session, L"Could not open SSH shell channel"));
            return false;
        }
        if (retryInt([this] {
                return libssh2_channel_request_pty_ex(
                    shell, shellOptions.terminalType.c_str(),
                    static_cast<unsigned int>(shellOptions.terminalType.size()),
                    nullptr, 0, shellOptions.columns, shellOptions.rows, 0, 0);
            }) != 0 ||
            retryInt([this] { return libssh2_channel_shell(shell); }) != 0) {
            setError(error, sshError(session, L"Could not start SSH PTY shell"));
            return false;
        }
        connected.store(true, std::memory_order_release);
        return true;
    }

    bool flushInput() {
        std::string pending;
        {
            std::lock_guard lock(queueMutex);
            pending.swap(inputQueue);
        }
        if (pending.empty()) return false;
        std::size_t offset = 0;
        while (offset < pending.size()) {
            const ssize_t written = libssh2_channel_write(
                shell, pending.data() + offset, pending.size() - offset);
            if (written == LIBSSH2_ERROR_EAGAIN) {
                std::lock_guard lock(queueMutex);
                inputQueue.insert(0, pending.data() + offset,
                                  pending.size() - offset);
                return false;
            }
            if (written <= 0) return false;
            offset += static_cast<std::size_t>(written);
        }
        return true;
    }

    bool applyResize() {
        std::optional<std::pair<int, int>> resize;
        {
            std::lock_guard lock(queueMutex);
            resize.swap(pendingResize);
        }
        if (!resize) return false;
        const int result = libssh2_channel_request_pty_size(
            shell, resize->first, resize->second);
        if (result == LIBSSH2_ERROR_EAGAIN) {
            std::lock_guard lock(queueMutex);
            pendingResize = resize;
            return false;
        }
        return result == 0;
    }

    void observeShellOutput(const char* data, std::size_t length) {
        if (!data || length == 0) return;
        shellTail.append(data, length);
        if (shellTail.size() > 8192)
            shellTail.erase(0, shellTail.size() - 8192);
        const std::size_t line = shellTail.find_last_of("\r\n");
        std::string prompt = line == std::string::npos
            ? shellTail : shellTail.substr(line + 1);
        while (!prompt.empty() &&
               (prompt.back() == ' ' || prompt.back() == '\t'))
            prompt.pop_back();
        if (!prompt.empty() && prompt.back() == '#' &&
            prompt.find("root@") != std::string::npos) {
            rootShellActive = true;
        } else if (!prompt.empty() && prompt.back() == '$' &&
                   prompt.find('@') != std::string::npos) {
            rootShellActive = false;
        }
    }

    bool readShell(bool* produced) {
        char buffer[8192];
        if (produced) *produced = false;
        for (int count = 0; count < 16; ++count) {
            const ssize_t read = libssh2_channel_read(shell, buffer, sizeof(buffer));
            if (read == LIBSSH2_ERROR_EAGAIN) break;
            if (read <= 0) {
                if (read < 0 || libssh2_channel_eof(shell)) return false;
                break;
            }
            if (produced) *produced = true;
            observeShellOutput(buffer, static_cast<std::size_t>(read));
            if (callbacks.onOutput) callbacks.onOutput(buffer, static_cast<size_t>(read));
        }
        return true;
    }

    bool stepDirectory() {
        if (!directoryJob) return false;
        DirectoryJob& job = *directoryJob;
        if (job.request.maximumEntries == 0) {
            job.result.ok = true;
            completeDirectory(std::move(job.result), job.request.id);
            directoryJob.reset();
            return true;
        }

        if (!job.rootAttempted) {
            job.rootAttempted = true;
            const std::string path = wideToUtf8(job.result.path);
            if (!path.empty()) {
                job.rootChannel = retryPointer(
                    [this] { return libssh2_channel_open_session(session); });
                if (job.rootChannel) {
                    const bool ptyOk = retryInt([&] {
                        return libssh2_channel_request_pty_ex(
                            job.rootChannel, "dumb", 4, nullptr, 0,
                            80, 24, 0, 0);
                    }) == 0;
                    const bool withPassword = rootShellActive &&
                        !sudoPassword.empty();
                    const std::string command = rootDirectoryCommand(
                        path, withPassword);
                    const bool execOk = ptyOk && retryInt([&] {
                        return libssh2_channel_exec(job.rootChannel,
                                                    command.c_str());
                    }) == 0;
                    if (execOk) {
                        job.rootActive = true;
                        job.rootOutput.clear();
                        if (withPassword) {
                            job.rootInput = sudoPassword + "\n";
                            job.rootInputOffset = 0;
                        }
                        return true;
                    }
                    closeRootChannel(job);
                }
            }
        }

        if (job.rootActive) {
            char buffer[8192];
            bool readAny = false;
            while (job.rootInputOffset < job.rootInput.size()) {
                const ssize_t written = libssh2_channel_write(
                    job.rootChannel, job.rootInput.data() +
                        job.rootInputOffset,
                    job.rootInput.size() - job.rootInputOffset);
                if (written == LIBSSH2_ERROR_EAGAIN) return readAny;
                if (written <= 0) {
                    closeRootChannel(job);
                    job.rootOutput.clear();
                    break;
                }
                job.rootInputOffset += static_cast<std::size_t>(written);
            }
            if (job.rootInputOffset == job.rootInput.size() &&
                !job.rootInput.empty()) {
                SecureZeroMemory(job.rootInput.data(), job.rootInput.size());
                job.rootInput.clear();
                job.rootInputOffset = 0;
            }
            if (!job.rootActive) {
                // Deliberately fall through to the normal SFTP path.
            } else {
            for (int count = 0; count < 16; ++count) {
                const ssize_t read = libssh2_channel_read(
                    job.rootChannel, buffer, sizeof(buffer));
                if (read == LIBSSH2_ERROR_EAGAIN) break;
                if (read < 0) break;
                if (read == 0) break;
                readAny = true;
                if (job.rootOutput.size() < 2 * 1024 * 1024) {
                    const std::size_t room = 2 * 1024 * 1024 -
                                             job.rootOutput.size();
                    job.rootOutput.append(buffer,
                                          std::min<std::size_t>(room,
                                              static_cast<std::size_t>(read)));
                }
            }
            for (int count = 0; count < 16; ++count) {
                const ssize_t read = libssh2_channel_read_stderr(
                    job.rootChannel, buffer, sizeof(buffer));
                if (read == LIBSSH2_ERROR_EAGAIN || read <= 0) break;
                readAny = true;
                if (job.rootOutput.size() < 2 * 1024 * 1024) {
                    const std::size_t room = 2 * 1024 * 1024 -
                                             job.rootOutput.size();
                    job.rootOutput.append(buffer,
                                          std::min<std::size_t>(room,
                                              static_cast<std::size_t>(read)));
                }
            }
            if (!libssh2_channel_eof(job.rootChannel)) return readAny;
            const int status = libssh2_channel_get_exit_status(
                job.rootChannel);
            closeRootChannel(job);
            if (status == 0) {
                parseRootListing(job);
                job.result.ok = true;
                completeDirectory(std::move(job.result), job.request.id);
                directoryJob.reset();
                return true;
            }
            job.rootOutput.clear();
            }
        }

        if (!sftp) {
            sftp = retryPointer([this] { return libssh2_sftp_init(session); }, 15000);
            if (!sftp) {
                job.result.error = sshError(session, L"Could not open SFTP subsystem");
                completeDirectory(std::move(job.result), job.request.id);
                directoryJob.reset();
                return true;
            }
        }
        if (!job.handle) {
            const std::string path = wideToUtf8(job.result.path);
            if (path.empty()) {
                job.result.error = L"SFTP path is not valid UTF-8";
                completeDirectory(std::move(job.result), job.request.id);
                directoryJob.reset();
                return true;
            }
            job.handle = retryPointer([&] {
                return libssh2_sftp_opendir(sftp, path.c_str());
            });
            if (!job.handle) {
                job.result.error = L"Could not open remote directory";
                completeDirectory(std::move(job.result), job.request.id);
                directoryJob.reset();
                return true;
            }
        }
        if (job.closePending || job.result.entries.size() >= job.request.maximumEntries) {
            const int closed = libssh2_sftp_closedir(job.handle);
            if (closed == LIBSSH2_ERROR_EAGAIN) return false;
            job.handle = nullptr;
            if (closed != 0) job.result.error = L"Could not close remote directory";
            if (job.result.error.empty()) job.result.ok = true;
            completeDirectory(std::move(job.result), job.request.id);
            directoryJob.reset();
            return true;
        }
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        const int length = libssh2_sftp_readdir(
            job.handle, job.name, sizeof(job.name) - 1, &attributes);
        if (length == LIBSSH2_ERROR_EAGAIN) return false;
        if (length == 0) {
            job.closePending = true;
            return true;
        }
        if (length < 0) {
            job.result.error = L"Could not read remote directory";
            job.closePending = true;
            return true;
        }
        job.name[std::min<int>(length, static_cast<int>(sizeof(job.name) - 1))] = '\0';
        const std::wstring name = utf8ToWide(job.name);
        if (!name.empty() && name != L"." && name != L"..") {
            SshDirectoryEntry entry;
            entry.name = name;
            entry.isDirectory =
                (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
                (attributes.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR;
            if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0)
                entry.size = static_cast<std::uint64_t>(attributes.filesize);
            job.result.entries.push_back(std::move(entry));
        }
        return true;
    }

    SshFileOperationResult performFileOperation(
        const FileOperationRequest& request) {
        SshFileOperationResult result;
        if (request.source.empty()) {
            result.error = L"The remote source path is empty.";
            return result;
        }
        if ((request.kind == SshFileOperationKind::Copy ||
             request.kind == SshFileOperationKind::Move ||
             request.kind == SshFileOperationKind::Rename) &&
            request.destination.empty()) {
            result.error = L"The remote destination path is empty.";
            return result;
        }
        if (!sftp)
            sftp = retryPointer([this] { return libssh2_sftp_init(session); },
                                15000);
        if (!sftp) {
            result.error = sshError(session, L"Could not open SFTP subsystem");
            return result;
        }

        const std::string source = wideToUtf8(request.source);
        const std::string destination = wideToUtf8(request.destination);
        if (source.empty() ||
            ((request.kind == SshFileOperationKind::Copy ||
              request.kind == SshFileOperationKind::Move ||
              request.kind == SshFileOperationKind::Rename) &&
             destination.empty())) {
            result.error = L"The remote path is not valid UTF-8.";
            return result;
        }

        auto sftpError = [this](const wchar_t* fallback) {
            return std::wstring(fallback) + L" (SFTP error " +
                   std::to_wstring(libssh2_sftp_last_error(sftp)) + L")";
        };
        auto isDirectory = [](const LIBSSH2_SFTP_ATTRIBUTES& attributes) {
            return (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
                   (attributes.permissions & LIBSSH2_SFTP_S_IFMT) ==
                       LIBSSH2_SFTP_S_IFDIR;
        };
        auto statPath = [&](const std::string& path,
                            LIBSSH2_SFTP_ATTRIBUTES& attributes) {
            std::memset(&attributes, 0, sizeof(attributes));
            return retryInt([&] {
                       return libssh2_sftp_lstat(sftp, path.c_str(),
                                                &attributes);
                   }) == 0;
        };
        auto joinPath = [](const std::string& parent,
                           const std::string& name) {
            if (parent.empty() || parent == "/") return std::string("/") + name;
            return parent.back() == '/' ? parent + name : parent + "/" + name;
        };

        std::function<bool(const std::string&, std::vector<std::pair<
            std::string, bool>>&, std::wstring&)> listDirectory;
        listDirectory = [&](const std::string& path,
                            std::vector<std::pair<std::string, bool>>& entries,
                            std::wstring& error) {
            LIBSSH2_SFTP_HANDLE* handle = retryPointer(
                [&] { return libssh2_sftp_opendir(sftp, path.c_str()); });
            if (!handle) {
                error = sftpError(L"Could not open remote directory");
                return false;
            }
            char name[4096]{};
            for (;;) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    error = L"SSH session stopped";
                    retryInt([&] { return libssh2_sftp_closedir(handle); },
                             2000, true);
                    return false;
                }
                LIBSSH2_SFTP_ATTRIBUTES attributes{};
                const int length = retryInt([&] {
                    return libssh2_sftp_readdir(handle, name,
                                               sizeof(name) - 1,
                                               &attributes);
                });
                if (length == 0) break;
                if (length < 0) {
                    error = sftpError(L"Could not read remote directory");
                    retryInt([&] { return libssh2_sftp_closedir(handle); },
                             2000, true);
                    return false;
                }
                name[std::min<int>(length,
                                   static_cast<int>(sizeof(name) - 1))] = '\0';
                if (std::strcmp(name, ".") != 0 &&
                    std::strcmp(name, "..") != 0)
                    entries.emplace_back(name, isDirectory(attributes));
            }
            if (retryInt([&] { return libssh2_sftp_closedir(handle); },
                         2000, true) != 0) {
                error = sftpError(L"Could not close remote directory");
                return false;
            }
            return true;
        };

        std::function<bool(const std::string&, const std::string&,
                           std::wstring&)> copyTree;
        copyTree = [&](const std::string& from, const std::string& to,
                       std::wstring& error) {
            if (stopRequested.load(std::memory_order_acquire)) {
                error = L"SSH session stopped";
                return false;
            }
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            if (!statPath(from, attributes)) {
                error = sftpError(L"Could not inspect remote source");
                return false;
            }
            if (isDirectory(attributes)) {
                if (retryInt([&] {
                        return libssh2_sftp_mkdir(sftp, to.c_str(), 0755);
                    }) != 0) {
                    error = sftpError(L"Could not create remote directory");
                    return false;
                }
                std::vector<std::pair<std::string, bool>> entries;
                if (!listDirectory(from, entries, error)) return false;
                for (const auto& entry : entries) {
                    if (stopRequested.load(std::memory_order_acquire)) {
                        error = L"SSH session stopped";
                        return false;
                    }
                    if (!copyTree(joinPath(from, entry.first),
                                  joinPath(to, entry.first), error))
                        return false;
                }
                return true;
            }

            LIBSSH2_SFTP_HANDLE* input = retryPointer([&] {
                return libssh2_sftp_open(sftp, from.c_str(),
                                         LIBSSH2_FXF_READ, 0);
            });
            if (!input) {
                error = sftpError(L"Could not open remote source");
                return false;
            }
            LIBSSH2_SFTP_HANDLE* output = retryPointer([&] {
                return libssh2_sftp_open(
                    sftp, to.c_str(),
                    LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                    0644);
            });
            if (!output) {
                retryInt([&] { return libssh2_sftp_close(input); }, 2000, true);
                error = sftpError(L"Could not create remote destination");
                return false;
            }

            char buffer[64 * 1024];
            bool ok = true;
            for (;;) {
                if (stopRequested.load(std::memory_order_acquire)) {
                    error = L"SSH session stopped";
                    ok = false;
                    break;
                }
                const int read = retryInt([&] {
                    return static_cast<int>(libssh2_sftp_read(
                        input, buffer, sizeof(buffer)));
                });
                if (read == 0) break;
                if (read < 0) {
                    error = sftpError(L"Could not read remote source");
                    ok = false;
                    break;
                }
                int offset = 0;
                while (offset < read) {
                    if (stopRequested.load(std::memory_order_acquire)) {
                        error = L"SSH session stopped";
                        ok = false;
                        break;
                    }
                    const int written = retryInt([&] {
                        return static_cast<int>(libssh2_sftp_write(
                            output, buffer + offset,
                            static_cast<std::size_t>(read - offset)));
                    });
                    if (written <= 0) {
                        error = sftpError(L"Could not write remote destination");
                        ok = false;
                        break;
                    }
                    offset += written;
                }
                if (!ok) break;
            }
            if (retryInt([&] { return libssh2_sftp_close(input); }, 2000, true) != 0 &&
                ok) {
                error = sftpError(L"Could not close remote source");
                ok = false;
            }
            if (retryInt([&] { return libssh2_sftp_close(output); }, 2000, true) != 0 &&
                ok) {
                error = sftpError(L"Could not close remote destination");
                ok = false;
            }
            return ok;
        };

        std::function<bool(const std::string&, std::wstring&)> deleteTree;
        deleteTree = [&](const std::string& path, std::wstring& error) {
            if (stopRequested.load(std::memory_order_acquire)) {
                error = L"SSH session stopped";
                return false;
            }
            LIBSSH2_SFTP_ATTRIBUTES attributes{};
            if (!statPath(path, attributes)) {
                error = sftpError(L"Could not inspect remote path");
                return false;
            }
            if (isDirectory(attributes)) {
                std::vector<std::pair<std::string, bool>> entries;
                if (!listDirectory(path, entries, error)) return false;
                for (const auto& entry : entries) {
                    if (stopRequested.load(std::memory_order_acquire)) {
                        error = L"SSH session stopped";
                        return false;
                    }
                    if (!deleteTree(joinPath(path, entry.first), error))
                        return false;
                }
                if (retryInt([&] {
                        return libssh2_sftp_rmdir(sftp, path.c_str());
                    }) != 0) {
                    error = sftpError(L"Could not remove remote directory");
                    return false;
                }
            } else if (retryInt([&] {
                           return libssh2_sftp_unlink(sftp, path.c_str());
                       }) != 0) {
                error = sftpError(L"Could not remove remote file");
                return false;
            }
            return true;
        };

        if (request.kind == SshFileOperationKind::Copy) {
            LIBSSH2_SFTP_ATTRIBUTES existing{};
            if (statPath(destination, existing)) {
                result.error = L"The remote destination already exists.";
                return result;
            }
            result.ok = copyTree(source, destination, result.error);
            return result;
        }
        if (request.kind == SshFileOperationKind::Delete) {
            result.ok = deleteTree(source, result.error);
            return result;
        }
        LIBSSH2_SFTP_ATTRIBUTES existing{};
        if (statPath(destination, existing)) {
            result.error = L"The remote destination already exists.";
            return result;
        }
        if (retryInt([&] {
                return libssh2_sftp_rename(sftp, source.c_str(),
                                           destination.c_str());
            }) != 0) {
            result.error = sftpError(request.kind == SshFileOperationKind::Rename
                                         ? L"Could not rename remote path"
                                         : L"Could not move remote path");
            return result;
        }
        result.ok = true;
        return result;
    }

    void run() {
        std::wstring error;
        if (!establish(&error)) {
            if (callbacks.onError && !stopRequested.load(std::memory_order_acquire))
                callbacks.onError(error);
            stopRequested.store(true, std::memory_order_release);
            const std::wstring reason =
                error.empty() ? L"SSH connection failed" : error;
            failQueuedDirectories(reason);
            failQueuedFileOperations(reason);
            closeResources();
            exited.store(true, std::memory_order_release);
            if (callbacks.onExit) callbacks.onExit();
            return;
        }

        while (!stopRequested.load(std::memory_order_acquire)) {
            bool didWork = false;
            didWork |= applyResize();
            didWork |= flushInput();
            bool shellProduced = false;
            if (!readShell(&shellProduced)) {
                if (callbacks.onError && !stopRequested.load(std::memory_order_acquire))
                    callbacks.onError(L"SSH shell connection closed");
                break;
            }
            didWork |= shellProduced;
            if (!directoryJob) {
                std::lock_guard lock(queueMutex);
                if (!directoryQueue.empty()) {
                    DirectoryRequest request = std::move(directoryQueue.front());
                    directoryQueue.pop_front();
                    DirectoryJob job;
                    job.request = std::move(request);
                    job.result.path = job.request.path.empty() ? L"." : job.request.path;
                    directoryJob = std::move(job);
                    didWork = true;
                }
            }
            if (directoryJob) didWork |= stepDirectory();
            if (!directoryJob) {
                std::optional<FileOperationRequest> request;
                {
                    std::lock_guard lock(queueMutex);
                    if (!fileOperationQueue.empty()) {
                        request = std::move(fileOperationQueue.front());
                        fileOperationQueue.pop_front();
                    }
                }
                if (request) {
                    completeFileOperation(performFileOperation(*request),
                                           request->id);
                    didWork = true;
                }
            }
            if (!didWork) waitForIo(50);
        }

        const std::wstring reason =
            stopRequested.load(std::memory_order_acquire)
                ? L"SSH session stopped"
                : L"SSH session disconnected";
        stopRequested.store(true, std::memory_order_release);
        failQueuedDirectories(reason);
        failQueuedFileOperations(reason);
        closeResources();
        exited.store(true, std::memory_order_release);
        if (callbacks.onExit) callbacks.onExit();
    }
};

SshConnection::SshConnection() : impl_(std::make_unique<Impl>()) {}

SshConnection::~SshConnection() = default;

bool SshConnection::start(const SshProfile& profile,
                          const SshCredentials& credentials,
                          const SshShellOptions& options,
                          SshConnectionCallbacks callbacks,
                          std::wstring* error) {
    if (!validSshProfile(profile)) {
        if (error) *error = L"The SSH profile is invalid";
        return false;
    }
    if (options.columns <= 0 || options.rows <= 0 || options.terminalType.empty()) {
        if (error) *error = L"Invalid SSH shell dimensions or terminal type";
        return false;
    }
    if (impl_->worker.joinable() || impl_->started.load(std::memory_order_acquire)) {
        if (error) *error = L"SSH connection is already running";
        return false;
    }
    impl_->profile = profile;
    impl_->credentials = credentials;
    impl_->shellOptions = options;
    impl_->callbacks = std::move(callbacks);
    impl_->stopRequested.store(false, std::memory_order_release);
    impl_->connected.store(false, std::memory_order_release);
    impl_->authenticated.store(false, std::memory_order_release);
    impl_->exited.store(false, std::memory_order_release);
    impl_->started.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->run(); });
    } catch (...) {
        impl_->started.store(false, std::memory_order_release);
        impl_->exited.store(true, std::memory_order_release);
        if (error) *error = L"Could not start the SSH worker thread";
        return false;
    }
    return true;
}

void SshConnection::disconnect() { impl_->stop(); }

bool SshConnection::isConnected() const {
    return impl_->connected.load(std::memory_order_acquire);
}

bool SshConnection::isAuthenticated() const {
    return impl_->authenticated.load(std::memory_order_acquire);
}

bool SshConnection::hasExited() const {
    return impl_->exited.load(std::memory_order_acquire);
}

bool SshConnection::writeShell(const char* data, std::size_t length,
                               std::wstring* error) {
    if (!data && length != 0) {
        if (error) *error = L"SSH input buffer is null";
        return false;
    }
    if (length == 0) return true;
    if (impl_->submitAuthInput(data, length)) return true;
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopRequested.load(std::memory_order_acquire)) {
        if (error) *error = L"SSH session is not running";
        return false;
    }
    constexpr std::size_t kMaxQueuedInput = 8 * 1024 * 1024;
    std::lock_guard lock(impl_->queueMutex);
    if (impl_->inputQueue.size() + length > kMaxQueuedInput) {
        if (error) *error = L"SSH input queue is full";
        return false;
    }
    impl_->inputQueue.append(data, length);
    impl_->queueCv.notify_one();
    return true;
}

bool SshConnection::resizeShell(int columns, int rows, std::wstring* error) {
    if (columns <= 0 || rows <= 0) {
        if (error) *error = L"Invalid SSH shell dimensions";
        return false;
    }
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopRequested.load(std::memory_order_acquire)) {
        if (error) *error = L"SSH session is not running";
        return false;
    }
    {
        std::lock_guard lock(impl_->queueMutex);
        impl_->pendingResize = std::make_pair(columns, rows);
    }
    impl_->queueCv.notify_one();
    return true;
}

SshDirectoryRequestId SshConnection::requestDirectory(
    const std::wstring& path, std::size_t maximumEntries) {
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopRequested.load(std::memory_order_acquire))
        return 0;
    std::lock_guard lock(impl_->queueMutex);
    const SshDirectoryRequestId id = impl_->nextRequestId++;
    impl_->directoryQueue.push_back({id, path, maximumEntries});
    impl_->queueCv.notify_one();
    return id;
}

std::optional<SshDirectoryListing> SshConnection::takeDirectoryResult(
    SshDirectoryRequestId requestId) {
    if (requestId == 0) return std::nullopt;
    std::lock_guard lock(impl_->resultMutex);
    const auto found = impl_->results.find(requestId);
    if (found == impl_->results.end()) return std::nullopt;
    SshDirectoryListing result = std::move(found->second);
    impl_->results.erase(found);
    return result;
}

SshFileOperationRequestId SshConnection::requestFileOperation(
    SshFileOperationKind kind, const std::wstring& source,
    const std::wstring& destination) {
    if (!impl_->started.load(std::memory_order_acquire) ||
        impl_->stopRequested.load(std::memory_order_acquire))
        return 0;
    std::lock_guard lock(impl_->queueMutex);
    const SshFileOperationRequestId id = impl_->nextRequestId++;
    impl_->fileOperationQueue.push_back({id, kind, source, destination});
    impl_->queueCv.notify_one();
    return id;
}

std::optional<SshFileOperationResult>
SshConnection::takeFileOperationResult(SshFileOperationRequestId requestId) {
    if (requestId == 0) return std::nullopt;
    std::lock_guard lock(impl_->fileOperationResultMutex);
    const auto found = impl_->fileOperationResults.find(requestId);
    if (found == impl_->fileOperationResults.end()) return std::nullopt;
    SshFileOperationResult result = std::move(found->second);
    impl_->fileOperationResults.erase(found);
    return result;
}

}  // namespace liney

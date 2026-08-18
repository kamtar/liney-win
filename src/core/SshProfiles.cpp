#include "core/SshProfiles.h"

#include <windows.h>

namespace liney {
namespace {
std::wstring quote(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'\"') result.append(slashes * 2 + 1, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring commandFor(const SshProfile& profile) {
    if (!validSshHost(profile.host) || !validSshUser(profile.user) ||
        profile.port < 1 || profile.port > 65535)
        return {};

    std::wstring command = L"ssh";
    command += L" -o StrictHostKeyChecking=ask -o UpdateHostKeys=yes";
    if (profile.port != 22)
        command += L" -p " + std::to_wstring(profile.port);
    if (!profile.identityFile.empty())
        command += L" -i " + quote(profile.identityFile);
    command += L" -- " + quote(sshProfileTarget(profile));
    return command;
}

} // namespace

bool validSshHost(const std::wstring& host) {
    if (host.empty() || host.size() > 512) return false;
    for (wchar_t ch : host) {
        const bool allowed = (ch >= L'a' && ch <= L'z') ||
                             (ch >= L'A' && ch <= L'Z') ||
                             (ch >= L'0' && ch <= L'9') || ch == L'.' ||
                             ch == L'-' || ch == L'_' ||
                             ch == L':' || ch == L'[' || ch == L']' ||
                             ch == L'%';
        if (!allowed) return false;
    }
    return true;
}

bool validSshUser(const std::wstring& user) {
    if (user.empty()) return true;
    if (user.size() > 128) return false;
    for (wchar_t ch : user) {
        const bool allowed = (ch >= L'a' && ch <= L'z') ||
                             (ch >= L'A' && ch <= L'Z') ||
                             (ch >= L'0' && ch <= L'9') || ch == L'.' ||
                             ch == L'-' || ch == L'_' || ch == L'+' ||
                             ch == L'@' || ch == L'%' || ch == L'\\';
        if (!allowed) return false;
    }
    return true;
}

bool parseSshPort(const std::wstring& text, int& port) {
    if (text.empty() || text.size() > 5) return false;
    int value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        value = value * 10 + static_cast<int>(ch - L'0');
        if (value > 65535) return false;
    }
    if (value < 1) return false;
    port = value;
    return true;
}

bool validSshProfile(const SshProfile& profile) {
    if (profile.name.empty() || profile.name.size() > 128) return false;
    bool nameHasVisibleCharacter = false;
    for (wchar_t ch : profile.name) {
        if (ch < 0x20 || ch == 0x7f) return false;
        if (ch != L' ' && ch != L'\t') nameHasVisibleCharacter = true;
    }
    if (!nameHasVisibleCharacter || !validSshHost(profile.host) ||
        !validSshUser(profile.user) || profile.port < 1 ||
        profile.port > 65535)
        return false;
    // Windows filenames cannot contain control characters. Rejecting them here
    // also keeps the quoted command-line argument unambiguous for hand-written
    // config values, while preserving support for spaces and Unicode paths.
    if (profile.identityFile.size() > 32767) return false;
    for (wchar_t ch : profile.identityFile)
        if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}

std::wstring sshProfileTarget(const SshProfile& profile) {
    if (profile.host.empty()) return {};
    return profile.user.empty() ? profile.host
                                : profile.user + L"@" + profile.host;
}

std::wstring buildSshCommand(const SshProfile& profile) {
    return commandFor(profile);
}

std::wstring buildSftpCommand(const SshProfile& profile) {
    if (!validSshHost(profile.host) || !validSshUser(profile.user) ||
        profile.port < 1 || profile.port > 65535)
        return {};
    std::wstring command = L"sftp -q -o BatchMode=yes";
    if (profile.port != 22)
        command += L" -P " + std::to_wstring(profile.port);
    if (!profile.identityFile.empty())
        command += L" -i " + quote(profile.identityFile);
    command += L" -b - " + quote(sshProfileTarget(profile));
    return command;
}

std::wstring buildSshDiagnosticCommand(const SshProfile& profile) {
    std::wstring command = buildSshCommand(profile);
    if (command.empty()) return {};
    // Batch mode never asks for or captures a password. Verbose OpenSSH output
    // explains DNS, host-key, key-file, agent and authentication failures.
    command.replace(0, 4,
                    L"ssh -vv -o BatchMode=yes -o ConnectTimeout=10 ");
    return command;
}

} // namespace liney

#pragma once

#include <string>

namespace liney {

struct SshProfile {
    std::wstring name;
    std::wstring host; // host name or address, without a user prefix
    int port = 22;
    std::wstring identityFile;
    std::wstring user; // optional SSH login name; empty means no user override
};

bool validSshHost(const std::wstring& host);
bool validSshUser(const std::wstring& user);
// Parse the decimal port text used by the add-profile prompt. Signs,
// whitespace, non-ASCII digits, overflow, and ports outside TCP's range are
// rejected so user input cannot silently turn into a different endpoint.
bool parseSshPort(const std::wstring& text, int& port);

// Validate the complete profile before it is added to user configuration.
// Command construction intentionally keeps its existing host/port checks;
// this stricter helper is for newly-created profiles and their display fields.
bool validSshProfile(const SshProfile& profile);

std::wstring sshProfileTarget(const SshProfile& profile);
std::wstring buildSshCommand(const SshProfile& profile);
// Legacy command construction retained for diagnostics/fallback. Normal SSH
// sessions use the embedded libssh2 transport, which shares authentication
// with the SFTP subsystem instead of spawning a second OpenSSH client.
std::wstring buildSftpCommand(const SshProfile& profile);
std::wstring buildSshDiagnosticCommand(const SshProfile& profile);

} // namespace liney

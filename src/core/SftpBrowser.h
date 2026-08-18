#pragma once

#include <string>
#include <vector>

#include "core/SshProfiles.h"

namespace liney {

struct RemoteFileEntry {
    std::wstring name;
    bool isDir = false;
};

struct SftpListing {
    bool ok = false;
    std::wstring path;
    std::wstring error;
    std::vector<RemoteFileEntry> entries;
};

// Legacy batch-mode helper. Active saved SSH sessions use SshConnection so the
// terminal and SFTP browser share one authenticated embedded transport.
SftpListing listSftpDirectory(const SshProfile& profile,
                              const std::wstring& path,
                              unsigned long timeoutMs = 15000);

}  // namespace liney

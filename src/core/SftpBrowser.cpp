#include "core/SftpBrowser.h"

#include "util/Process.h"

#include <windows.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <utility>

namespace liney {
namespace {

std::wstring quoteSftpArg(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'\"' || ch == L'\\') result.push_back(L'\\');
        result.push_back(ch);
    }
    result.push_back(L'\"');
    return result;
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), bytes,
                        nullptr, nullptr);
    return result;
}

std::wstring trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool skipField(const std::wstring& line, size_t& pos) {
    while (pos < line.size() && (line[pos] == L' ' || line[pos] == L'\t')) ++pos;
    if (pos >= line.size()) return false;
    while (pos < line.size() && line[pos] != L' ' && line[pos] != L'\t') ++pos;
    return true;
}

std::wstring lastOutputLine(const std::wstring& output) {
    size_t end = output.size();
    while (end > 0 && (output[end - 1] == L'\r' || output[end - 1] == L'\n' ||
                       output[end - 1] == L' ' || output[end - 1] == L'\t'))
        --end;
    const size_t delimiter = end == 0
        ? std::wstring::npos
        : output.find_last_of(L"\r\n", end - 1);
    const size_t start = delimiter == std::wstring::npos ? 0 : delimiter + 1;
    return trim(output.substr(start, end - start));
}

}  // namespace

SftpListing listSftpDirectory(const SshProfile& profile,
                              const std::wstring& path,
                              unsigned long timeoutMs) {
    SftpListing result;
    const std::wstring command = buildSftpCommand(profile);
    if (command.empty()) {
        result.error = L"The SSH profile is invalid.";
        return result;
    }

    std::wstring batch;
    if (!path.empty()) batch += L"cd " + quoteSftpArg(path) + L"\n";
    batch += L"pwd\nls -la\nquit\n";
    bool processOk = false;
    const std::wstring output = runCaptureWithInput(
        command, L"", toUtf8(batch), &processOk, timeoutMs);
    if (!processOk) {
        result.error = L"SFTP could not authenticate without prompting. "
                       L"Configure an SSH key or agent for this host.";
        const std::wstring detail = lastOutputLine(output);
        if (!detail.empty() && detail.find(L"sftp>") == std::wstring::npos)
            result.error += L" (" + detail + L")";
        return result;
    }

    size_t lineStart = 0;
    while (lineStart <= output.size()) {
        size_t lineEnd = output.find(L'\n', lineStart);
        if (lineEnd == std::wstring::npos) lineEnd = output.size();
        const std::wstring line = trim(output.substr(lineStart, lineEnd - lineStart));
        constexpr const wchar_t* kPrefix = L"Remote working directory: ";
        if (line.rfind(kPrefix, 0) == 0) {
            result.path = trim(line.substr(std::wcslen(kPrefix)));
        } else if (line.size() > 1 &&
                   (line[0] == L'd' || line[0] == L'-' || line[0] == L'l' ||
                    line[0] == L'b' || line[0] == L'c' || line[0] == L'p' ||
                    line[0] == L's')) {
            // OpenSSH's `ls -l` output has eight whitespace-delimited fields
            // before the filename. The filename itself is left untouched, so
            // spaces in normal remote names remain usable in the panel.
            size_t nameStart = 0;
            bool fieldsPresent = true;
            for (int field = 0; field < 8; ++field)
                if (!skipField(line, nameStart)) {
                    fieldsPresent = false;
                    break;
                }
            if (fieldsPresent) {
                const std::wstring name = trim(line.substr(nameStart));
                if (!name.empty() && name != L"." && name != L"..") {
                    RemoteFileEntry entry;
                    entry.isDir = line[0] == L'd';
                    entry.name = name;
                    if (line[0] == L'l') {
                        const size_t arrow = entry.name.find(L" -> ");
                        if (arrow != std::wstring::npos)
                            entry.name.resize(arrow);
                    }
                    if (!entry.name.empty()) result.entries.push_back(std::move(entry));
                }
            }
        }
        if (lineEnd == output.size()) break;
        lineStart = lineEnd + 1;
    }

    if (result.path.empty()) result.path = path.empty() ? L"." : path;
    std::sort(result.entries.begin(), result.entries.end(),
              [](const RemoteFileEntry& left, const RemoteFileEntry& right) {
                  if (left.isDir != right.isDir) return left.isDir;
                  return _wcsicmp(left.name.c_str(), right.name.c_str()) < 0;
              });
    if (result.entries.size() > 500) result.entries.resize(500);
    result.ok = true;
    return result;
}

}  // namespace liney

#include "core/Update.h"

#include <cctype>
#include <limits>
#include <sstream>

namespace liney {
namespace {

bool parseVersion(const std::string& s, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
    for (int part = 0; part < 3; ++part) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
        int value = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            const int digit = s[i++] - '0';
            if (value > (std::numeric_limits<int>::max() - digit) / 10)
                return false;
            value = value * 10 + digit;
        }
        out[part] = value;
        if (part < 2) {
            if (i >= s.size() || s[i] != '.') return false;
            ++i;
        }
    }
    return i == s.size();
}

} // namespace

bool versionNewer(const std::string& remote, const std::string& local) {
    int r[3], l[3];
    if (!parseVersion(remote, r) || !parseVersion(local, l)) return false;
    for (int i = 0; i < 3; ++i)
        if (r[i] != l[i]) return r[i] > l[i];
    return false;
}

bool isValidSha256(const std::string& digest) {
    if (digest.size() != 64) return false;
    for (char ch : digest)
        if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    return true;
}

bool parseTrustedInstallerUrl(const std::wstring& url, std::wstring& host,
                              std::wstring& path) {
    host.clear();
    path.clear();
    static constexpr wchar_t prefix[] =
        L"https://github.com/kamtar/liney-win/releases/download/";
    if (url.rfind(prefix, 0) != 0) return false;
    if (url.find_first_of(L"?#") != std::wstring::npos) return false;
    const size_t pathStart = std::wstring(L"https://github.com").size();
    if (url.size() <= pathStart || url[pathStart] != L'/') return false;
    host = L"github.com";
    path = url.substr(pathStart);
    return path.find(L"..") == std::wstring::npos &&
           path.find(L'\\') == std::wstring::npos;
}

std::string parseReleaseSha256(const std::string& manifest,
                               const std::string& assetName) {
    if (assetName.empty() || assetName.find_first_of("/\\\r\n") !=
                                 std::string::npos)
        return {};

    std::istringstream lines(manifest);
    std::string line;
    std::string result;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 67) continue;

        const std::string digest = line.substr(0, 64);
        if (!isValidSha256(digest) || line[64] != ' ') continue;

        size_t nameStart = 65;
        if (nameStart < line.size() &&
            (line[nameStart] == ' ' || line[nameStart] == '*'))
            ++nameStart;
        if (line.substr(nameStart) != assetName) continue;
        if (!result.empty()) return {};  // ambiguous duplicate entry

        result = digest;
        for (char& ch : result)
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

bool updatePreservesPublisherTrust(bool currentSigned, bool candidateSigned,
                                   bool samePublisher) {
    return !currentSigned || (candidateSigned && samePublisher);
}

} // namespace liney

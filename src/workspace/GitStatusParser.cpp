#include "workspace/GitStatusParser.h"

#include <cerrno>
#include <climits>
#include <cwchar>

namespace liney {

GitWorktreeStatus parseGitStatusPorcelainV2(const std::wstring& text) {
    GitWorktreeStatus result;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.rfind(L"# branch.head ", 0) == 0) {
            result.branch = line.substr(14);
            result.detached = result.branch == L"(detached)";
        } else if (line.rfind(L"# branch.ab ", 0) == 0) {
            const wchar_t* p = line.c_str() + 12;
            if (*p++ == L'+' && *p >= L'0' && *p <= L'9') {
                errno = 0;
                wchar_t* end = nullptr;
                const unsigned long aheadValue = wcstoul(p, &end, 10);
                const bool aheadOk = end != p && *end == L' ' &&
                    errno != ERANGE && aheadValue <= INT_MAX;
                if (aheadOk) {
                    p = end + 1;
                    if (*p++ == L'-' && *p >= L'0' && *p <= L'9') {
                        errno = 0;
                        const unsigned long behindValue = wcstoul(p, &end, 10);
                        const bool behindOk = end != p && *end == L'\0' &&
                            errno != ERANGE && behindValue <= INT_MAX;
                        if (behindOk) {
                            result.ahead = static_cast<int>(aheadValue);
                            result.behind = static_cast<int>(behindValue);
                        }
                    }
                }
            }
        } else if (!line.empty() &&
                   (line[0] == L'1' || line[0] == L'2' || line[0] == L'u' ||
                    line[0] == L'?' || line[0] == L'!')) {
            // Ignored files are deliberately not dirty state.
            if (line[0] != L'!') ++result.changed;
        }
        if (end == text.size()) break;
        pos = end + 1;
    }
    return result;
}

} // namespace liney

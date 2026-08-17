#include "core/PowerShellHistory.h"

#include <algorithm>
#include <cstdint>

namespace liney {
namespace {

std::wstring normalizeForIdentity(const std::wstring& input) {
    std::wstring path = input;
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 1 && path.back() == L'\\') {
        if (path.size() == 3 && path[1] == L':') break;
        path.pop_back();
    }
    for (wchar_t& ch : path)
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch + 32);
    return path;
}

uint64_t fnv1a(const std::wstring& value) {
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t ch : value) {
        hash ^= static_cast<uint64_t>(static_cast<uint32_t>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

std::wstring hex64(uint64_t value) {
    static constexpr wchar_t digits[] = L"0123456789abcdef";
    std::wstring out(16, L'0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = digits[value & 0xf];
        value >>= 4;
    }
    return out;
}

std::wstring safeBaseName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring source =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::wstring out;
    for (wchar_t ch : source) {
        const bool safe = (ch >= L'a' && ch <= L'z') ||
                          (ch >= L'A' && ch <= L'Z') ||
                          (ch >= L'0' && ch <= L'9') || ch == L'-' ||
                          ch == L'_' || ch == L'.';
        out.push_back(safe ? ch : L'_');
        if (out.size() >= 40) break;
    }
    return out.empty() ? L"project" : out;
}

}  // namespace

std::wstring powerShellHistoryIdentity(const std::wstring& projectPath,
                                       const std::wstring& worktreePath) {
    if (!worktreePath.empty())
        return L"worktree:" + normalizeForIdentity(worktreePath);
    if (!projectPath.empty())
        return L"project:" + normalizeForIdentity(projectPath);
    return L"";
}

std::wstring powerShellHistoryFileName(const std::wstring& projectPath,
                                       const std::wstring& worktreePath) {
    const std::wstring identity =
        powerShellHistoryIdentity(projectPath, worktreePath);
    if (identity.empty()) return L"";
    const std::wstring source =
        worktreePath.empty() ? projectPath : worktreePath;
    return safeBaseName(source) + L"-" + hex64(fnv1a(identity)) + L".txt";
}

}  // namespace liney

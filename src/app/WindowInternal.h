#pragma once

// Shared internals for the Window implementation, which is split across several
// translation units (Window.cpp + Window*.cpp). Small file-local helpers and
// the chrome palette live here so each unit can use them.

#include <windows.h>

#include <limits>
#include <string>

#include "render/Cell.h"

namespace liney {

inline constexpr const wchar_t* kAppVersion = L"0.10.10";  // sync with AppxManifest

// Stable, high-contrast project colors used by the workspace rail and tab
// indicators. The assignment is persisted per project path by Window.
inline constexpr Color kProjectPalette[] = {
    {120, 200, 160}, {130, 170, 230}, {220, 170, 110},
    {200, 140, 200}, {210, 130, 130}, {150, 190, 120},
    {100, 190, 190}, {220, 190, 100}
};
inline constexpr Color kArchivedProjectColor{128, 128, 128};
inline constexpr Color kNeutralUiColor{145, 145, 145};

// Chrome colors are now runtime-themeable (Window::uiTheme_, see core/Themes.h).
// The drawing code refers to uiTheme_.sidebarBg / .accent / … directly.

inline std::wstring parentDir(const std::wstring& path) {
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) --end;
    size_t slash = path.find_last_of(L"\\/", end ? end - 1 : 0);
    if (slash == std::wstring::npos) return path;
    return path.substr(0, slash);
}

inline bool keyDown(int vk) { return (GetKeyState(vk) & 0x8000) != 0; }

// The current user's home directory (%USERPROFILE%, e.g. C:\Users\name), used
// as the default working directory for new terminals.
inline std::wstring homeDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    n = GetEnvironmentVariableW(L"HOMEDRIVE", buf, MAX_PATH);  // fallback
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\" : L"C:\\";
}

inline std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    if (w.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};
    const int length = static_cast<int>(w.size());
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), length,
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), length,
                            s.data(), n, nullptr, nullptr) != n)
        return {};
    return s;
}

inline std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    if (s.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        return {};
    const int length = static_cast<int>(s.size());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), length,
                                nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), length,
                            w.data(), n) != n)
        return {};
    return w;
}

} // namespace liney

#include "util/Url.h"

#include <cwctype>
#include <utility>

namespace liney {
namespace {

bool startsWithInsensitive(const std::wstring& text, size_t at,
                           const wchar_t* prefix) {
    for (size_t i = 0; prefix[i] != L'\0'; ++i) {
        if (at + i >= text.size() ||
            towlower(text[at + i]) != towlower(prefix[i]))
            return false;
    }
    return true;
}

bool isUrlBoundary(wchar_t c) {
    return !(iswalnum(c) || c == L'_' || c == L'-' || c == L'.');
}

bool endsWithClosingDelimiter(const std::wstring& url, wchar_t closing,
                              wchar_t opening) {
    if (url.empty() || url.back() != closing) return false;
    size_t opens = 0;
    size_t closes = 0;
    for (wchar_t c : url) {
        if (c == opening) ++opens;
        else if (c == closing) ++closes;
    }
    return closes > opens;
}

void trimUrlPunctuation(std::wstring& url) {
    while (!url.empty()) {
        const wchar_t c = url.back();
        if (c == L'.' || c == L',' || c == L';' || c == L':' ||
            c == L'!' || c == L'?') {
            url.pop_back();
            continue;
        }
        if (endsWithClosingDelimiter(url, L')', L'(') ||
            endsWithClosingDelimiter(url, L']', L'[') ||
            endsWithClosingDelimiter(url, L'}', L'{')) {
            url.pop_back();
            continue;
        }
        break;
    }
}

}  // namespace

std::vector<UrlSpan> detectHttpUrls(const std::wstring& text) {
    std::vector<UrlSpan> result;
    constexpr wchar_t kHttp[] = L"http://";
    constexpr wchar_t kHttps[] = L"https://";

    size_t search = 0;
    while (search < text.size()) {
        const size_t http = text.find_first_of(L"hH", search);
        if (http == std::wstring::npos) break;
        search = http + 4;

        if (http > 0 && !isUrlBoundary(text[http - 1])) continue;

        size_t schemeLength = 0;
        if (startsWithInsensitive(text, http, kHttps))
            schemeLength = sizeof(kHttps) / sizeof(kHttps[0]) - 1;
        else if (startsWithInsensitive(text, http, kHttp))
            schemeLength = sizeof(kHttp) / sizeof(kHttp[0]) - 1;
        else
            continue;

        size_t end = http + schemeLength;
        while (end < text.size()) {
            const wchar_t c = text[end];
            if (iswspace(c) || iswcntrl(c) || c == L'<' || c == L'>' ||
                c == L'"' || c == L'\'' || c == L'`')
                break;
            ++end;
        }

        std::wstring url = text.substr(http, end - http);
        trimUrlPunctuation(url);
        if (url.size() <= schemeLength) continue;

        result.push_back({http, http + url.size(), std::move(url)});
        search = http + url.size();
    }
    return result;
}

}  // namespace liney

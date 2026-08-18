#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace liney {

// A URL span uses UTF-16 code-unit offsets into the source text. The terminal
// grid adapter converts these offsets back to terminal cell columns.
struct UrlSpan {
    size_t start = 0;
    size_t end = 0;  // exclusive
    std::wstring url;
};

// Detect plain-text HTTP(S) URLs. This intentionally excludes other schemes:
// terminal output is untrusted, and only web links should be handled by the
// terminal's click affordance.
std::vector<UrlSpan> detectHttpUrls(const std::wstring& text);

}  // namespace liney

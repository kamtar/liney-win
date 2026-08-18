#include "core/SerialProfiles.h"

#include <algorithm>

namespace liney {
namespace {

constexpr uint32_t kMaxSerialBaud = 4'000'000;
constexpr size_t kMaxProfileName = 128;
constexpr size_t kMaxHexInputBytes = 1024 * 1024;

bool startsWithDevicePrefix(const std::wstring& value) {
    return value.size() >= 4 && value[0] == L'\\' && value[1] == L'\\' &&
           value[2] == L'.' && value[3] == L'\\';
}

bool asciiInsensitiveEqual(wchar_t left, wchar_t right) {
    if (left >= L'a' && left <= L'z') left -= L'a' - L'A';
    if (right >= L'a' && right <= L'z') right -= L'a' - L'A';
    return left == right;
}

bool isComPrefix(const std::wstring& value) {
    return value.size() >= 3 && asciiInsensitiveEqual(value[0], L'C') &&
           asciiInsensitiveEqual(value[1], L'O') &&
           asciiInsensitiveEqual(value[2], L'M');
}

std::wstring portStem(const std::wstring& port) {
    return startsWithDevicePrefix(port) ? port.substr(4) : port;
}

bool setError(std::wstring* error, const wchar_t* message) {
    if (error) *error = message;
    return false;
}

int hexValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return static_cast<int>(ch - L'0');
    if (ch >= L'a' && ch <= L'f') return static_cast<int>(ch - L'a' + 10);
    if (ch >= L'A' && ch <= L'F') return static_cast<int>(ch - L'A' + 10);
    return -1;
}

bool isAsciiWhitespace(wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
}

void appendHexOffset(std::string& out, uint64_t offset) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        out.push_back(kHex[(offset >> shift) & 0xf]);
}

} // namespace

std::wstring canonicalSerialPortName(const std::wstring& port) {
    if (!validSerialPortName(port)) return {};
    std::wstring result = L"\\\\.\\";
    const std::wstring stem = portStem(port);
    result.reserve(result.size() + stem.size());
    for (wchar_t ch : stem) {
        if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
        result.push_back(ch);
    }
    return result;
}

bool validSerialPortName(const std::wstring& port) {
    if (port.empty() || port.size() > 16) return false;
    const std::wstring stem = portStem(port);
    if (stem.size() < 4 || !isComPrefix(stem)) return false;

    unsigned long number = 0;
    for (size_t i = 3; i < stem.size(); ++i) {
        const wchar_t ch = stem[i];
        if (ch < L'0' || ch > L'9') return false;
        number = number * 10 + static_cast<unsigned long>(ch - L'0');
        if (number > 256) return false;
    }
    return number >= 1;
}

std::wstring serialProfileDisplayName(const SerialProfile& profile) {
    std::wstring port = portStem(profile.port);
    for (wchar_t& ch : port)
        if (ch >= L'a' && ch <= L'z') ch -= L'a' - L'A';
    const std::wstring generated =
        port + L", " + std::to_wstring(profile.baudRate);
    if (profile.name.empty() || profile.name.size() == generated.size()) {
        bool generatedName = profile.name.size() == generated.size();
        if (generatedName)
            for (size_t i = 0; i < generated.size(); ++i)
                if (!asciiInsensitiveEqual(profile.name[i], generated[i])) {
                    generatedName = false;
                    break;
                }
        if (profile.name.empty() || generatedName) return generated;
    }
    return profile.name + L", " + generated;
}

bool validSerialProfile(const SerialProfile& profile, std::wstring* error) {
    if (profile.name.size() > kMaxProfileName)
        return setError(error, L"The serial profile name is too long.");
    for (wchar_t ch : profile.name) {
        if (ch == L'\0' || ch == L'\r' || ch == L'\n' ||
            ch < 0x20)
            return setError(error, L"The serial profile name contains control characters.");
    }
    if (!validSerialPortName(profile.port))
        return setError(error, L"The serial port must be COM1 through COM256.");
    if (profile.baudRate == 0 || profile.baudRate > kMaxSerialBaud)
        return setError(error, L"The baud rate must be between 1 and 4000000.");
    if (profile.dataBits < 5 || profile.dataBits > 8)
        return setError(error, L"Data bits must be between 5 and 8.");

    switch (profile.parity) {
    case SerialParity::None:
    case SerialParity::Odd:
    case SerialParity::Even:
    case SerialParity::Mark:
    case SerialParity::Space:
        break;
    default:
        return setError(error, L"The selected parity mode is invalid.");
    }
    switch (profile.stopBits) {
    case SerialStopBits::One:
    case SerialStopBits::OnePointFive:
    case SerialStopBits::Two:
        break;
    default:
        return setError(error, L"The selected stop-bit mode is invalid.");
    }
    switch (profile.mode) {
    case SerialMode::Terminal:
    case SerialMode::RawText:
    case SerialMode::RawHexMonitor:
        break;
    default:
        return setError(error, L"The selected serial display mode is invalid.");
    }
    switch (profile.lineEnding) {
    case SerialLineEnding::CarriageReturn:
    case SerialLineEnding::LineFeed:
    case SerialLineEnding::CarriageReturnLineFeed:
    case SerialLineEnding::None:
        break;
    default:
        return setError(error, L"The selected serial line ending is invalid.");
    }

    if (profile.stopBits == SerialStopBits::OnePointFive &&
        profile.dataBits != 5)
        return setError(error, L"1.5 stop bits requires 5 data bits.");
    if (profile.stopBits == SerialStopBits::Two && profile.dataBits == 5)
        return setError(error, L"2 stop bits cannot be used with 5 data bits.");
    return true;
}

bool parseSerialHexInput(const std::wstring& input, std::string& bytes,
                         std::wstring* error) {
    bytes.clear();
    int highNibble = -1;
    for (wchar_t ch : input) {
        if (isAsciiWhitespace(ch)) {
            if (highNibble >= 0)
                return setError(error, L"Whitespace may appear only between complete bytes.");
            continue;
        }
        const int nibble = hexValue(ch);
        if (nibble < 0)
            return setError(error, L"Hex input may contain only 0-9, A-F, and whitespace.");
        if (highNibble < 0) {
            highNibble = nibble;
        } else {
            if (bytes.size() >= kMaxHexInputBytes)
                return setError(error, L"Hex input is limited to 1 MiB.");
            bytes.push_back(static_cast<char>((highNibble << 4) | nibble));
            highNibble = -1;
        }
    }
    if (highNibble >= 0)
        return setError(error, L"Hex input must contain complete byte pairs.");
    if (bytes.empty()) return setError(error, L"Hex input is empty.");
    return true;
}

std::string formatSerialHexDump(const char* data, size_t len,
                                uint64_t& offset) {
    if (!data || len == 0) return {};
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(((len + 15) / 16) * 82);
    size_t consumed = 0;
    while (consumed < len) {
        const size_t rowBytes = std::min<size_t>(16, len - consumed);
        appendHexOffset(result, offset);
        result += "  ";
        for (size_t i = 0; i < 16; ++i) {
            if (i < rowBytes) {
                const unsigned char byte =
                    static_cast<unsigned char>(data[consumed + i]);
                result.push_back(kHex[byte >> 4]);
                result.push_back(kHex[byte & 0xf]);
            } else {
                result += "  ";
            }
            if (i != 15) result.push_back(' ');
        }
        result += " |";
        for (size_t i = 0; i < 16; ++i) {
            if (i >= rowBytes) {
                result.push_back(' ');
                continue;
            }
            const unsigned char byte =
                static_cast<unsigned char>(data[consumed + i]);
            result.push_back(byte >= 0x20 && byte <= 0x7e
                                 ? static_cast<char>(byte)
                                 : '.');
        }
        result += "|\r\n";
        consumed += rowBytes;
        offset += rowBytes;
    }
    return result;
}

std::string formatSerialText(const char* data, size_t len) {
    if (!data || len == 0) return {};
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const unsigned char byte = static_cast<unsigned char>(data[i]);
        if (byte == '\r' || byte == '\n' || byte == '\t' ||
            (byte >= 0x20 && byte <= 0x7e) || byte >= 0x80) {
            result.push_back(static_cast<char>(byte));
        } else if (byte == 0x7f) {
            result += "^?";
        } else {
            result.push_back('^');
            result.push_back(static_cast<char>('@' + byte));
        }
    }
    return result;
}

} // namespace liney

#pragma once

#include <cstdint>
#include <string>

namespace liney {

// The Windows DCB supports these five parity modes. Keeping the model free of
// Win32 types lets dialogs validate and persist profiles without pulling in
// the serial transport implementation.
enum class SerialParity { None, Odd, Even, Mark, Space };

enum class SerialStopBits { One, OnePointFive, Two };

// Terminal mode keeps the normal VT behavior. RawHexMonitor is intended for
// devices such as GPS receivers and modems whose bytes must be inspected
// without allowing arbitrary input to act as terminal control sequences.
// Keep the original numeric values stable because layout.json stores this
// enum numerically; RawText is appended after the existing two modes.
enum class SerialMode { Terminal = 0, RawHexMonitor = 1, RawText = 2 };

// The line ending appended when RawText sends a line on Enter.
enum class SerialLineEnding { CarriageReturn, LineFeed, CarriageReturnLineFeed, None };

struct SerialProfile {
    std::wstring name;
    // Accepts COM3 and the canonical \\.\COM3 spelling. Other device paths
    // are deliberately rejected so a dialog cannot turn this into an arbitrary
    // file/device opener.
    std::wstring port;
    uint32_t baudRate = 115200;
    uint8_t dataBits = 8;
    SerialParity parity = SerialParity::None;
    SerialStopBits stopBits = SerialStopBits::One;
    SerialMode mode = SerialMode::Terminal;
    SerialLineEnding lineEnding = SerialLineEnding::CarriageReturn;
};

// Sidebar/tab label: "COM9, 9600" or "GPS, COM9, 9600". Legacy generated
// names are recognized so older profiles do not display the port twice.
std::wstring serialProfileDisplayName(const SerialProfile& profile);

// Validate the user-facing port name and return its canonical CreateFileW
// spelling (\\.\COM10). Returns empty for invalid input.
std::wstring canonicalSerialPortName(const std::wstring& port);
bool validSerialPortName(const std::wstring& port);

// Returns false for unsupported DCB combinations and optionally supplies a
// concise dialog-ready error. No device is opened by this function.
bool validSerialProfile(const SerialProfile& profile,
                        std::wstring* error = nullptr);

// Parse an outgoing hex command. Only hex digit pairs with optional ASCII
// whitespace between them are accepted (no 0x prefix, commas, or comments).
// The limit prevents a dialog action from queuing unbounded device input.
bool parseSerialHexInput(const std::wstring& input, std::string& bytes,
                         std::wstring* error = nullptr);

// Convert raw input to printable, fixed-width rows. Each row contains a
// 32-bit display offset, 16 hex bytes, and a 16-character ASCII column. The
// offset is advanced by the number of bytes consumed, including partial rows.
std::string formatSerialHexDump(const char* data, size_t len,
                                uint64_t& offset);

// Make incoming bytes safe for the plain-text serial view. Printable UTF-8
// bytes and ordinary whitespace remain readable; C0/control bytes become
// caret notation instead of being interpreted as terminal escapes.
std::string formatSerialText(const char* data, size_t len);

} // namespace liney

#include "serial/SerialPort.h"

#include <algorithm>
#include <cwchar>
#include <utility>
#include <vector>

namespace liney {
namespace {

constexpr size_t kMaxWriteQueueBytes = 4 * 1024 * 1024;

std::wstring formatWindowsError(DWORD code) {
    if (code == ERROR_SUCCESS) return {};
    wchar_t* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD chars = FormatMessageW(
        flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0,
        nullptr);
    std::wstring result = chars && buffer ? std::wstring(buffer, chars)
                                          : L"Windows serial I/O failed.";
    if (buffer) LocalFree(buffer);
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ' || result.back() == L'\t'))
        result.pop_back();
    return result;
}

BYTE toWinParity(SerialParity parity) {
    switch (parity) {
    case SerialParity::Odd: return ODDPARITY;
    case SerialParity::Even: return EVENPARITY;
    case SerialParity::Mark: return MARKPARITY;
    case SerialParity::Space: return SPACEPARITY;
    case SerialParity::None: return NOPARITY;
    }
    return NOPARITY;
}

BYTE toWinStopBits(SerialStopBits stopBits) {
    switch (stopBits) {
    case SerialStopBits::One: return ONESTOPBIT;
    case SerialStopBits::OnePointFive: return ONE5STOPBITS;
    case SerialStopBits::Two: return TWOSTOPBITS;
    }
    return ONESTOPBIT;
}

bool closeIfInvalid(HANDLE& handle) {
    if (handle == INVALID_HANDLE_VALUE) {
        handle = nullptr;
        return true;
    }
    return false;
}

} // namespace

SerialPort::~SerialPort() { stop(); }

void SerialPort::setError(DWORD code) {
    errorCode_ = code;
    std::lock_guard<std::mutex> lock(errorMutex_);
    errorMessage_ = formatWindowsError(code);
}

std::wstring SerialPort::errorMessage() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return errorMessage_;
}

bool SerialPort::start(const SerialProfile& profile, OutputHandler onOutput,
                       ExitHandler onExit) {
    stop();
    setError(ERROR_SUCCESS);
    onOutput_ = std::move(onOutput);
    onExit_ = std::move(onExit);

    std::wstring validationError;
    const std::wstring device = canonicalSerialPortName(profile.port);
    if (!validSerialProfile(profile, &validationError)) {
        {
            std::lock_guard<std::mutex> lock(errorMutex_);
            errorMessage_ = std::move(validationError);
        }
        errorCode_ = ERROR_INVALID_PARAMETER;
        return false;
    }

    handle_ = CreateFileW(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (closeIfInvalid(handle_)) {
        setError(GetLastError());
        return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(handle_, &dcb)) {
        setError(GetLastError());
        stop();
        return false;
    }
    dcb.BaudRate = profile.baudRate;
    dcb.ByteSize = profile.dataBits;
    dcb.Parity = toWinParity(profile.parity);
    dcb.StopBits = toWinStopBits(profile.stopBits);
    dcb.fBinary = TRUE;
    dcb.fParity = profile.parity != SerialParity::None;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(handle_, &dcb)) {
        setError(GetLastError());
        stop();
        return false;
    }

    // A short constant timeout makes a blocking reader interruptible even on
    // drivers that do not promptly honor CancelIoEx. With no bytes pending,
    // ReadFile returns within 100 ms; available bytes are still delivered as
    // one or more chunks without polling the UI thread.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutConstant = 5000;
    if (!SetCommTimeouts(handle_, &timeouts) ||
        !SetupComm(handle_, 64 * 1024, 64 * 1024) ||
        !PurgeComm(handle_, PURGE_RXABORT | PURGE_RXCLEAR | PURGE_TXABORT |
                              PURGE_TXCLEAR)) {
        setError(GetLastError());
        stop();
        return false;
    }

    exited_ = false;
    running_ = true;
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        writeQueue_.clear();
        writeStop_ = false;
    }

    readThread_ = std::thread([this]() {
        std::vector<char> buffer(4096);
        while (running_) {
            DWORD read = 0;
            if (!ReadFile(handle_, buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                const DWORD error = GetLastError();
                if (running_ && error != ERROR_OPERATION_ABORTED)
                    setError(error);
                break;
            }
            if (running_ && read != 0 && onOutput_)
                onOutput_(buffer.data(), read);
        }
        running_ = false;
        if (handle_) CancelIoEx(handle_, nullptr);
        writeCv_.notify_one();
        exited_ = true;
        if (onExit_) onExit_();
    });

    writeThread_ = std::thread([this]() {
        std::string pending;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(writeMutex_);
                writeCv_.wait(lock, [this] {
                    return writeStop_ || !running_ || !writeQueue_.empty();
                });
                if (writeStop_ || !running_) return;
                pending.clear();
                pending.swap(writeQueue_);
            }

            size_t offset = 0;
            while (offset < pending.size() && running_) {
                DWORD written = 0;
                if (!WriteFile(handle_, pending.data() + offset,
                               static_cast<DWORD>(pending.size() - offset),
                               &written, nullptr) ||
                    written == 0) {
                    const DWORD error = GetLastError();
                    if (running_ && error != ERROR_OPERATION_ABORTED)
                        setError(error);
                    running_ = false;
                    writeCv_.notify_one();
                    return;
                }
                offset += written;
            }
        }
    });
    return true;
}

void SerialPort::write(const char* data, size_t len) {
    if (!data || len == 0 || !running_) return;
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (writeStop_ || !running_ || writeQueue_.size() > kMaxWriteQueueBytes ||
            len > kMaxWriteQueueBytes - writeQueue_.size())
            return;
        writeQueue_.append(data, len);
    }
    writeCv_.notify_one();
}

void SerialPort::stop() {
    running_ = false;
    if (handle_) CancelIoEx(handle_, nullptr);
    {
        std::lock_guard<std::mutex> lock(writeMutex_);
        writeStop_ = true;
        writeQueue_.clear();
    }
    writeCv_.notify_one();

    if (readThread_.joinable()) readThread_.join();
    if (writeThread_.joinable()) writeThread_.join();

    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    exited_ = true;
}

} // namespace liney

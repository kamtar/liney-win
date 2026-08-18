#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "core/SerialProfiles.h"

namespace liney {

// Windows COM transport for TerminalSession. It intentionally exposes the
// same byte-stream shape as ConPty: one reader callback, an asynchronous input
// queue, and bounded teardown. Serial data is passed directly to the VT core;
// no ConPTY or child process is created.
class SerialPort {
public:
    using OutputHandler = std::function<void(const char* data, size_t len)>;
    using ExitHandler = std::function<void()>;

    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    // Opens and configures the COM port before returning. Output is delivered
    // from the reader thread; onExit is called once when the port disconnects
    // or stop() ends the reader.
    bool start(const SerialProfile& profile, OutputHandler onOutput,
               ExitHandler onExit = nullptr);

    // Queue bytes for the device without blocking the UI thread.
    void write(const char* data, size_t len);
    void stop();

    bool hasExited() const { return exited_.load(); }
    // Serial sessions have no child process and no process exit code.
    bool hasRunningChild() const { return false; }
    unsigned long processId() const { return 0; }

    // Start/open and subsequent I/O failures are available to the parent for
    // a dialog/toast. ERROR_SUCCESS means no failure has been observed.
    DWORD errorCode() const { return errorCode_.load(); }
    std::wstring errorMessage() const;

private:
    void setError(DWORD code);

    HANDLE handle_ = nullptr;
    std::thread readThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> exited_{true};
    std::atomic<DWORD> errorCode_{ERROR_SUCCESS};
    OutputHandler onOutput_;
    ExitHandler onExit_;

    std::thread writeThread_;
    std::mutex writeMutex_;
    std::condition_variable writeCv_;
    std::string writeQueue_;
    bool writeStop_ = false;

    mutable std::mutex errorMutex_;
    std::wstring errorMessage_;
};

} // namespace liney

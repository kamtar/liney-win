#include "util/ConnectionDialog.h"

#include <dwmapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <string>
#include <vector>

#include "util/Dialogs.h"

namespace liney {
namespace {

constexpr int kIdHost = 100;
constexpr int kIdPort = 101;
constexpr int kIdBaud = 102;
constexpr int kIdName = 103;
constexpr int kIdIdentity = 104;
constexpr int kIdBrowse = 105;
constexpr int kIdDataBits = 106;
constexpr int kIdParity = 107;
constexpr int kIdStopBits = 108;
constexpr int kIdMode = 109;
constexpr int kIdLineEnding = 110;
constexpr int kIdUser = 111;
constexpr int kIdValidation = 112;

struct State {
    ConnectionDialogKind kind = ConnectionDialogKind::Serial;
    ConnectionDialogValues* values = nullptr;
    UiTheme uiTheme{};
    HWND host = nullptr;
    HWND user = nullptr;
    HWND port = nullptr;
    HWND baud = nullptr;
    HWND name = nullptr;
    HWND identity = nullptr;
    HWND dataBits = nullptr;
    HWND parity = nullptr;
    HWND stopBits = nullptr;
    HWND mode = nullptr;
    HWND lineEnding = nullptr;
    HWND validation = nullptr;
    HBRUSH backgroundBrush = nullptr;
    HBRUSH fieldBrush = nullptr;
    bool settingName = false;
    bool nameAutomatic = true;
    bool done = false;
    bool accepted = false;
};

COLORREF rgb(const Color& color) {
    return RGB(color.r, color.g, color.b);
}

std::wstring windowText(HWND window) {
    if (!window) return {};
    const int length = GetWindowTextLengthW(window);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(window, value.data(), length + 1);
    value.resize(static_cast<size_t>(length));
    return value;
}

void addComboItem(HWND combo, const std::wstring& value) {
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(value.c_str()));
}

void selectComboValue(HWND combo, const std::wstring& value) {
    const LRESULT index = SendMessageW(
        combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1),
        reinterpret_cast<LPARAM>(value.c_str()));
    if (index != CB_ERR) SendMessageW(combo, CB_SETCURSEL, index, 0);
    else SetWindowTextW(combo, value.c_str());
}

std::wstring displaySerialPort(const std::wstring& port) {
    if (port.size() > 4 && port[0] == L'\\' && port[1] == L'\\' &&
        port[2] == L'.' && port[3] == L'\\')
        return port.substr(4);
    return port;
}

int serialPortNumber(const std::wstring& port) {
    const std::wstring display = displaySerialPort(port);
    if (display.size() < 4 ||
        (display[0] != L'C' && display[0] != L'c') ||
        (display[1] != L'O' && display[1] != L'o') ||
        (display[2] != L'M' && display[2] != L'm'))
        return 0;
    int number = 0;
    for (size_t i = 3; i < display.size(); ++i) {
        if (display[i] < L'0' || display[i] > L'9') return 0;
        number = number * 10 + static_cast<int>(display[i] - L'0');
    }
    return number;
}

std::vector<std::wstring> detectedSerialPorts() {
    std::vector<std::wstring> ports;
    for (int number = 1; number <= 256; ++number) {
        const std::wstring port = L"COM" + std::to_wstring(number);
        wchar_t target[512]{};
        if (QueryDosDeviceW(port.c_str(), target, _countof(target)) != 0)
            ports.push_back(port);
    }
    std::sort(ports.begin(), ports.end(), [](const auto& left, const auto& right) {
        return serialPortNumber(left) < serialPortNumber(right);
    });
    return ports;
}

std::vector<std::wstring> standardBaudRates() {
    return {L"110", L"300", L"600", L"1200", L"2400", L"4800",
            L"9600", L"14400", L"19200", L"28800", L"38400", L"57600",
            L"115200", L"230400", L"460800", L"921600", L"1000000",
            L"2000000", L"4000000"};
}

bool parseUnsigned(const std::wstring& text, uint32_t maximum,
                   uint32_t& value) {
    if (text.empty()) return false;
    uint64_t parsed = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
        parsed = parsed * 10 + static_cast<uint64_t>(ch - L'0');
        if (parsed > maximum) return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

std::wstring generatedName(const State* state) {
    if (!state) return {};
    if (state->kind == ConnectionDialogKind::Serial) {
        const std::wstring port = windowText(state->port);
        const std::wstring baud = windowText(state->baud);
        if (port.empty() || baud.empty()) return {};
        return displaySerialPort(port) + L", " + baud;
    }
    SshProfile profile;
    profile.host = windowText(state->host);
    profile.user = windowText(state->user);
    return sshProfileTarget(profile);
}

void updateGeneratedName(State* state) {
    if (!state || !state->name || !state->nameAutomatic) return;
    const std::wstring value = generatedName(state);
    state->settingName = true;
    SetWindowTextW(state->name, value.c_str());
    state->settingName = false;
}

void showValidation(State* state, HWND focus, const std::wstring& message) {
    if (!state || !state->validation) return;
    SetWindowTextW(state->validation, message.c_str());
    ShowWindow(state->validation, SW_SHOW);
    if (focus) {
        SetFocus(focus);
        if (GetWindowLongPtrW(focus, GWL_STYLE) & ES_AUTOHSCROLL)
            SendMessageW(focus, EM_SETSEL, 0, -1);
    }
}

void clearValidation(State* state) {
    if (!state || !state->validation) return;
    SetWindowTextW(state->validation, L"");
    ShowWindow(state->validation, SW_HIDE);
}

bool selectedIndex(HWND combo, int& index) {
    if (!combo) return false;
    const LRESULT selected = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) return false;
    index = static_cast<int>(selected);
    return true;
}

bool collectAndValidate(State* state) {
    if (!state || !state->values) return false;
    clearValidation(state);

    if (state->kind == ConnectionDialogKind::Ssh) {
        SshProfile profile;
        profile.host = windowText(state->host);
        profile.user = windowText(state->user);
        profile.name = windowText(state->name);
        if (profile.name.empty()) profile.name = sshProfileTarget(profile);
        profile.identityFile = windowText(state->identity);
        if (profile.host.empty()) {
            showValidation(state, state->host, L"Host is required.");
            return false;
        }
        if (!validSshUser(profile.user)) {
            showValidation(state, state->user,
                           L"User contains an unsupported character.");
            return false;
        }
        if (!parseSshPort(windowText(state->port), profile.port)) {
            showValidation(state, state->port,
                           L"Port must be a number from 1 through 65535.");
            return false;
        }
        if (!validSshHost(profile.host)) {
            showValidation(state, state->host,
                           L"Enter a host name or address without user@.");
            return false;
        }
        if (!validSshProfile(profile)) {
            showValidation(state, state->name,
                           L"Enter a valid display name and identity path.");
            return false;
        }
        state->values->ssh = std::move(profile);
        return true;
    }

    SerialProfile profile;
    profile.port = windowText(state->port);
    profile.name = windowText(state->name);
    uint32_t number = 0;
    if (!validSerialPortName(profile.port)) {
        showValidation(state, state->port, L"Choose a valid COM port.");
        return false;
    }
    if (!parseUnsigned(windowText(state->baud), 4'000'000, number) ||
        number == 0) {
        showValidation(state, state->baud,
                       L"Baud rate must be between 1 and 4000000.");
        return false;
    }
    profile.baudRate = number;
    if (!parseUnsigned(windowText(state->dataBits), 8, number) || number < 5) {
        showValidation(state, state->dataBits,
                       L"Data bits must be 5, 6, 7, or 8.");
        return false;
    }
    profile.dataBits = static_cast<uint8_t>(number);
    int index = 0;
    if (!selectedIndex(state->parity, index) || index < 0 || index > 4) {
        showValidation(state, state->parity, L"Choose a parity mode.");
        return false;
    }
    profile.parity = static_cast<SerialParity>(index);
    if (!selectedIndex(state->stopBits, index) || index < 0 || index > 2) {
        showValidation(state, state->stopBits, L"Choose a stop-bit mode.");
        return false;
    }
    profile.stopBits = static_cast<SerialStopBits>(index);
    if (!selectedIndex(state->mode, index) || index < 0 || index > 2) {
        showValidation(state, state->mode, L"Choose a display mode.");
        return false;
    }
    profile.mode = index == 0 ? SerialMode::Terminal
                   : index == 1 ? SerialMode::RawText
                                 : SerialMode::RawHexMonitor;
    if (!selectedIndex(state->lineEnding, index) || index < 0 || index > 3) {
        showValidation(state, state->lineEnding, L"Choose a line ending.");
        return false;
    }
    profile.lineEnding = static_cast<SerialLineEnding>(index);
    std::wstring validationError;
    if (!validSerialProfile(profile, &validationError)) {
        showValidation(state, state->dataBits, validationError);
        return false;
    }
    state->values->serial = std::move(profile);
    return true;
}

LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<State*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wParam) == IDOK) {
            if (collectAndValidate(state)) {
                state->accepted = true;
                state->done = true;
            }
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            state->done = true;
            return 0;
        }
        if (LOWORD(wParam) == kIdBrowse && HIWORD(wParam) == BN_CLICKED) {
            const wchar_t filter[] =
                L"SSH private key\0*.*\0All files\0*.*\0";
            const std::wstring path = pickFile(
                hwnd, L"Select SSH identity file",
                std::wstring(filter, _countof(filter)));
            if (!path.empty()) SetWindowTextW(state->identity, path.c_str());
            return 0;
        }
        if (LOWORD(wParam) == kIdName && HIWORD(wParam) == EN_CHANGE) {
            if (!state->settingName) state->nameAutomatic = false;
            return 0;
        }
        if (((LOWORD(wParam) == kIdHost || LOWORD(wParam) == kIdUser) &&
             HIWORD(wParam) == EN_CHANGE) ||
            ((LOWORD(wParam) == kIdPort || LOWORD(wParam) == kIdBaud) &&
             HIWORD(wParam) == CBN_SELCHANGE)) {
            updateGeneratedName(state);
            clearValidation(state);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, rgb(state->uiTheme.text));
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(state->backgroundBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, rgb(state->uiTheme.text));
        SetBkColor(dc, rgb(state->uiTheme.tabActiveBg));
        return reinterpret_cast<LRESULT>(state->fieldBrush);
    }
    case WM_ERASEBKGND: {
        if (!state || !state->backgroundBrush) break;
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, state->backgroundBrush);
        return 1;
    }
    case WM_CLOSE:
        if (state) state->done = true;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

} // namespace

bool showConnectionDialog(HWND owner, ConnectionDialogKind kind,
                          ConnectionDialogValues& values,
                          const UiTheme& uiTheme) {
    static const wchar_t* kClass = L"LineyConnectionDialog";
    HINSTANCE instance = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    const UINT dpi = owner ? GetDpiForWindow(owner) : 96;
    auto scale = [dpi](int value) {
        return MulDiv(value, static_cast<int>(dpi), 96);
    };
    const bool serial = kind == ConnectionDialogKind::Serial;
    const int clientWidth = 520;
    const int clientHeight = serial ? 436 : 328;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT windowRect{0, 0, scale(clientWidth), scale(clientHeight)};
    if (!AdjustWindowRectExForDpi(&windowRect, style, FALSE,
                                  WS_EX_DLGMODALFRAME, dpi))
        AdjustWindowRectEx(&windowRect, style, FALSE, WS_EX_DLGMODALFRAME);
    const int windowWidth = windowRect.right - windowRect.left;
    const int windowHeight = windowRect.bottom - windowRect.top;

    RECT ownerRect{200, 200, 900, 700};
    if (owner) GetWindowRect(owner, &ownerRect);
    const int x = ownerRect.left +
        ((ownerRect.right - ownerRect.left) - windowWidth) / 2;
    const int y = ownerRect.top +
        ((ownerRect.bottom - ownerRect.top) - windowHeight) / 2;
    const std::wstring title = serial ? L"Add serial connection"
                                      : L"Add SSH connection";
    HWND dialog = CreateWindowExW(
        WS_EX_DLGMODALFRAME, kClass, title.c_str(), style, x, y,
        windowWidth, windowHeight, owner, nullptr, instance, nullptr);
    if (!dialog) return false;

    State state;
    state.kind = kind;
    state.values = &values;
    state.uiTheme = uiTheme;
    state.backgroundBrush = CreateSolidBrush(rgb(uiTheme.sidebarBg));
    state.fieldBrush = CreateSolidBrush(rgb(uiTheme.tabActiveBg));
    SetWindowLongPtrW(dialog, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(&state));

    BOOL dark = TRUE;
    DwmSetWindowAttribute(dialog, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                          &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(dialog, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corners, sizeof(corners));

    const int margin = 24;
    const int labelWidth = 122;
    const int controlX = margin + labelWidth + 12;
    const int controlRight = clientWidth - margin;
    const int controlWidth = controlRight - controlX;
    const int rowHeight = 26;
    auto createControl = [&](DWORD ex, const wchar_t* className,
                             const wchar_t* text, DWORD windowStyle,
                             int left, int top, int width, int height,
                             int id) -> HWND {
        return CreateWindowExW(
            ex, className, text, windowStyle, scale(left), scale(top),
            scale(width), scale(height), dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
            nullptr);
    };
    auto label = [&](const wchar_t* text, int top) {
        createControl(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                      margin, top + 4, labelWidth, 18, -1);
    };
    auto heading = [&](const wchar_t* text, int top) {
        createControl(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                      margin, top, clientWidth - 2 * margin, 22, -1);
    };
    auto edit = [&](const wchar_t* text, int top, int width, int id) {
        return createControl(
            WS_EX_CLIENTEDGE, L"EDIT", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            controlX, top, width, rowHeight, id);
    };
    auto combo = [&](int top, int width, int id, DWORD extra = 0) {
        return createControl(
            WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                WS_VSCROLL | extra,
            controlX, top, width, 220, id);
    };

    heading(serial ? L"Serial terminal" : L"SSH terminal", 18);
    if (serial) {
        SerialProfile profile = values.serial;
        std::vector<std::wstring> ports = detectedSerialPorts();
        const std::wstring currentPort = displaySerialPort(profile.port);
        if (validSerialPortName(currentPort) &&
            std::find(ports.begin(), ports.end(), currentPort) == ports.end())
            ports.push_back(currentPort);
        if (ports.empty()) ports.push_back(L"COM1");
        std::sort(ports.begin(), ports.end(), [](const auto& left,
                                                 const auto& right) {
            return serialPortNumber(left) < serialPortNumber(right);
        });

        label(L"Port", 52);
        state.port = combo(52, 190, kIdPort);
        for (const auto& port : ports) addComboItem(state.port, port);
        const std::wstring selectedPort = currentPort.empty() ? ports.front()
                                                               : currentPort;
        selectComboValue(state.port, selectedPort);

        label(L"Baud rate", 88);
        state.baud = combo(88, 190, kIdBaud);
        for (const auto& baud : standardBaudRates()) addComboItem(state.baud, baud);
        const uint32_t baudValue = profile.baudRate == 0 ? 9600 : profile.baudRate;
        selectComboValue(state.baud, std::to_wstring(baudValue));

        label(L"Name (optional)", 124);
        state.name = edit(L"", 124, controlWidth, kIdName);
        state.nameAutomatic = false;
        state.settingName = true;
        const std::wstring generated = displaySerialPort(profile.port) +
            L", " + std::to_wstring(profile.baudRate);
        const bool legacyGeneratedName = profile.name == generated;
        SetWindowTextW(state.name,
                       legacyGeneratedName ? L"" : profile.name.c_str());
        state.settingName = false;

        heading(L"Advanced", 162);
        label(L"Data bits", 190);
        state.dataBits = combo(190, 100, kIdDataBits);
        for (const wchar_t* value : {L"5", L"6", L"7", L"8"})
            addComboItem(state.dataBits, value);
        selectComboValue(state.dataBits,
                         std::to_wstring(profile.dataBits < 5 || profile.dataBits > 8
                                             ? 8
                                             : profile.dataBits));

        label(L"Parity", 226);
        state.parity = combo(226, 190, kIdParity);
        for (const wchar_t* value : {L"None", L"Odd", L"Even", L"Mark", L"Space"})
            addComboItem(state.parity, value);
        SendMessageW(state.parity, CB_SETCURSEL,
                     static_cast<int>(profile.parity) <= 4
                         ? static_cast<int>(profile.parity) : 0,
                     0);

        label(L"Stop bits", 262);
        state.stopBits = combo(262, 190, kIdStopBits);
        for (const wchar_t* value : {L"1", L"1.5", L"2"})
            addComboItem(state.stopBits, value);
        SendMessageW(state.stopBits, CB_SETCURSEL,
                     static_cast<int>(profile.stopBits) <= 2
                         ? static_cast<int>(profile.stopBits) : 0,
                     0);

        label(L"Display", 298);
        state.mode = combo(298, controlWidth, kIdMode);
        addComboItem(state.mode, L"Terminal");
        addComboItem(state.mode, L"Raw text (send on Enter)");
        addComboItem(state.mode, L"Raw hex monitor");
        SendMessageW(state.mode, CB_SETCURSEL,
                     profile.mode == SerialMode::RawHexMonitor ? 2
                         : profile.mode == SerialMode::RawText ? 1 : 0,
                     0);

        label(L"Line ending", 334);
        state.lineEnding = combo(334, 190, kIdLineEnding);
        addComboItem(state.lineEnding, L"CR");
        addComboItem(state.lineEnding, L"LF");
        addComboItem(state.lineEnding, L"CRLF");
        addComboItem(state.lineEnding, L"None");
        const int lineEnding = static_cast<int>(profile.lineEnding);
        SendMessageW(state.lineEnding, CB_SETCURSEL,
                     lineEnding >= 0 && lineEnding <= 3 ? lineEnding : 0, 0);
    } else {
        SshProfile profile = values.ssh;
        label(L"Host", 52);
        state.host = edit(profile.host.c_str(), 52, controlWidth, kIdHost);
        SendMessageW(state.host, EM_LIMITTEXT, 1024, 0);

        label(L"User (optional)", 88);
        state.user = edit(profile.user.c_str(), 88, controlWidth, kIdUser);
        SendMessageW(state.user, EM_LIMITTEXT, 128, 0);

        label(L"Port", 124);
        state.port = createControl(
            WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWN |
                CBS_AUTOHSCROLL | WS_VSCROLL,
            controlX, 124, 190, 220, kIdPort);
        for (const wchar_t* value : {L"22", L"2222", L"2022", L"2200"})
            addComboItem(state.port, value);
        selectComboValue(state.port,
                         std::to_wstring(profile.port == 0 ? 22 : profile.port));

        label(L"Name (optional)", 160);
        state.name = edit(profile.name.c_str(), 160, controlWidth, kIdName);
        const std::wstring generated = sshProfileTarget(profile);
        state.nameAutomatic = profile.name.empty() || profile.name == generated;
        SendMessageW(state.name, EM_LIMITTEXT, 128, 0);

        label(L"Identity file (optional)", 196);
        state.identity = edit(profile.identityFile.c_str(), 196,
                              controlWidth - 92, kIdIdentity);
        SendMessageW(state.identity, EM_LIMITTEXT, 32767, 0);
        createControl(0, L"BUTTON", L"Browse…",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      controlRight - 82, 196, 82, rowHeight, kIdBrowse);
        createControl(0, L"STATIC",
                      L"Leave blank to use OpenSSH agent/default key lookup.",
                      WS_CHILD | WS_VISIBLE, controlX, 228, controlWidth, 18,
                      -1);
    }

    state.validation = createControl(
        0, L"STATIC", L"", WS_CHILD | SS_LEFT,
        margin, serial ? 366 : 252, clientWidth - 2 * margin, 26,
        kIdValidation);
    ShowWindow(state.validation, SW_HIDE);

    const int buttonTop = serial ? 394 : 284;
    createControl(0, L"BUTTON", serial ? L"Add & connect" : L"Connect",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                  clientWidth - 248, buttonTop, 136, 28, IDOK);
    createControl(0, L"BUTTON", L"Cancel",
                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                  clientWidth - 104, buttonTop, 80, 28, IDCANCEL);

    // Match SettingsDialog: DEFAULT_GUI_FONT is an old bitmap stock font and
    // becomes disproportionately large on some per-monitor-DPI setups.
    HFONT font = CreateFontW(
        -MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
        FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    if (!font) font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    EnumChildWindows(dialog, [](HWND child, LPARAM parameter) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(parameter), TRUE);
        SetWindowTheme(child, L"DarkMode_Explorer", nullptr);
        return TRUE;
    }, reinterpret_cast<LPARAM>(font));
    updateGeneratedName(&state);

    ShowWindow(dialog, SW_SHOW);
    HWND initialFocus = serial ? state.port : state.host;
    SetFocus(initialFocus);
    if (owner) EnableWindow(owner, FALSE);

    MSG message{};
    BOOL result;
    while (!state.done && (result = GetMessageW(&message, nullptr, 0, 0)) != 0) {
        if (result == -1) break;
        if (!IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!state.done && message.message == WM_QUIT)
        PostQuitMessage(static_cast<int>(message.wParam));

    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dialog);
    if (state.backgroundBrush) DeleteObject(state.backgroundBrush);
    if (state.fieldBrush) DeleteObject(state.fieldBrush);
    if (font && font != reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)))
        DeleteObject(font);
    if (owner) SetForegroundWindow(owner);
    return state.accepted;
}

} // namespace liney

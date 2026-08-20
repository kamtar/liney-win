#include "core/Config.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include "util/Json.h"

namespace liney {

namespace {

constexpr int kConfigSchemaVersion = 1;

void configWarning(const std::wstring& message, const wchar_t* title) {
    wchar_t headless[8]{};
    if (GetEnvironmentVariableW(L"LINEY_HEADLESS", headless,
                                static_cast<DWORD>(_countof(headless))) > 0)
        return;
    MessageBoxW(nullptr, message.c_str(), title, MB_OK | MB_ICONWARNING);
}

std::wstring utf8ToWide(const std::string& s) {
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

std::string wideToUtf8(const std::wstring& w) {
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

bool splitLegacySshTarget(const std::wstring& value, std::wstring& user,
                          std::wstring& host) {
    const size_t at = value.find(L'@');
    if (at == std::wstring::npos) {
        user.clear();
        host = value;
        return validSshHost(host);
    }
    user = value.substr(0, at);
    host = value.substr(at + 1);
    return validSshUser(user) && !user.empty() && validSshHost(host);
}

Color hexToColor(const std::string& s, Color dflt) {
    std::string h = (!s.empty() && s[0] == '#') ? s.substr(1) : s;
    if (h.size() != 6) return dflt;
    auto hx = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int r0 = hx(h[0]), r1 = hx(h[1]), g0 = hx(h[2]),
              g1 = hx(h[3]), b0 = hx(h[4]), b1 = hx(h[5]);
    if (r0 < 0 || r1 < 0 || g0 < 0 || g1 < 0 || b0 < 0 || b1 < 0)
        return dflt;
    return { static_cast<uint8_t>(r0 * 16 + r1),
             static_cast<uint8_t>(g0 * 16 + g1),
             static_cast<uint8_t>(b0 * 16 + b1) };
}

std::string colorToHex(const Color& c) {
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

const char* serialParityName(SerialParity parity) {
    switch (parity) {
    case SerialParity::Odd: return "odd";
    case SerialParity::Even: return "even";
    case SerialParity::Mark: return "mark";
    case SerialParity::Space: return "space";
    case SerialParity::None: return "none";
    }
    return "none";
}

const char* serialStopBitsName(SerialStopBits stopBits) {
    switch (stopBits) {
    case SerialStopBits::OnePointFive: return "1.5";
    case SerialStopBits::Two: return "2";
    case SerialStopBits::One: return "1";
    }
    return "1";
}

const char* serialModeName(SerialMode mode) {
    if (mode == SerialMode::RawText) return "rawText";
    return mode == SerialMode::RawHexMonitor ? "rawHex" : "terminal";
}

const char* serialLineEndingName(SerialLineEnding ending) {
    switch (ending) {
    case SerialLineEnding::LineFeed: return "lf";
    case SerialLineEnding::CarriageReturnLineFeed: return "crlf";
    case SerialLineEnding::None: return "none";
    case SerialLineEnding::CarriageReturn: return "cr";
    }
    return "cr";
}

bool parseSerialParity(const std::string& value, SerialParity& parity) {
    if (value == "odd") parity = SerialParity::Odd;
    else if (value == "even") parity = SerialParity::Even;
    else if (value == "mark") parity = SerialParity::Mark;
    else if (value == "space") parity = SerialParity::Space;
    else if (value.empty() || value == "none") parity = SerialParity::None;
    else return false;
    return true;
}

bool parseSerialStopBits(const std::string& value, SerialStopBits& stopBits) {
    if (value == "1.5") stopBits = SerialStopBits::OnePointFive;
    else if (value == "2") stopBits = SerialStopBits::Two;
    else if (value.empty() || value == "1") stopBits = SerialStopBits::One;
    else return false;
    return true;
}

bool parseSerialMode(const std::string& value, SerialMode& mode) {
    if (value == "rawHex" || value == "raw" || value == "hex")
        mode = SerialMode::RawHexMonitor;
    else if (value == "rawText" || value == "text")
        mode = SerialMode::RawText;
    else if (value.empty() || value == "terminal")
        mode = SerialMode::Terminal;
    else
        return false;
    return true;
}

bool parseSerialLineEnding(const std::string& value,
                           SerialLineEnding& ending) {
    if (value == "lf") ending = SerialLineEnding::LineFeed;
    else if (value == "crlf") ending = SerialLineEnding::CarriageReturnLineFeed;
    else if (value == "none") ending = SerialLineEnding::None;
    else if (value.empty() || value == "cr") ending = SerialLineEnding::CarriageReturn;
    else return false;
    return true;
}

bool jsonUnsigned(const Json& value, uint32_t max, uint32_t& result) {
    if (value.type() != Json::Type::Number) return false;
    const double number = value.asNumber();
    if (!std::isfinite(number) || number < 0.0 || number > max ||
        std::floor(number) != number)
        return false;
    result = static_cast<uint32_t>(number);
    return true;
}

bool jsonInteger(const Json& value, int min, int max, int& result) {
    if (value.type() != Json::Type::Number) return false;
    const double number = value.asNumber();
    if (!std::isfinite(number) || std::floor(number) != number ||
        number < min || number > max)
        return false;
    result = static_cast<int>(number);
    return true;
}

bool jsonFloat(const Json& value, float min, float max, float& result) {
    if (value.type() != Json::Type::Number) return false;
    const double number = value.asNumber();
    if (!std::isfinite(number) || number < min || number > max)
        return false;
    const float converted = static_cast<float>(number);
    if (!std::isfinite(converted)) return false;
    result = converted;
    return true;
}

std::string readFile(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::wstring& path, const std::string& content) {
    return writeFileAtomic(path, content);
}

std::string defaultJson(const Config& c) {
    Json j = Json::object();
    j.set("schemaVersion", Json::number(kConfigSchemaVersion));
    j.set("shell", Json::str(wideToUtf8(c.shell)));
    j.set("fontFamily", Json::str(wideToUtf8(c.fontFamily)));
    j.set("fontSize", Json::number(c.fontSize));
    j.set("fontLigatures", Json::boolean(c.fontLigatures));
    j.set("scrollback", Json::number(c.scrollback));
    j.set("workspaceRoot", Json::str(wideToUtf8(c.workspaceRoot)));
    j.set("unixTools", Json::boolean(c.unixTools));
    j.set("copyOnSelect", Json::boolean(c.copyOnSelect));
    j.set("multiLinePasteWarning", Json::boolean(c.multiLinePasteWarning));
    j.set("rememberLayout", Json::boolean(c.rememberLayout));
    j.set("rememberPanelLayout", Json::boolean(c.rememberPanelLayout));
    j.set("sidebarVisible", Json::boolean(c.sidebarVisible));
    j.set("filesPanelVisible", Json::boolean(c.filesPanelVisible));
    j.set("sidebarWidth", Json::number(c.sidebarWidth));
    j.set("filesPanelWidth", Json::number(c.filesPanelWidth));
    j.set("splitUseWorkspaceDir", Json::boolean(c.splitUseWorkspaceDir));
    j.set("powerShellHistoryPerProject",
          Json::boolean(c.powerShellHistoryPerProject));
    j.set("checkForUpdatesOnStartup", Json::boolean(c.checkForUpdatesOnStartup));
    Json ai = Json::object();
    ai.set("provider", Json::str(wideToUtf8(c.aiProvider)));
    ai.set("model", Json::str(wideToUtf8(c.aiModel)));
    ai.set("endpoint", Json::str(wideToUtf8(c.aiEndpoint)));
    ai.set("includeCwd", Json::boolean(c.aiIncludeCwd));
    j.set("ai", std::move(ai));
    j.set("osc52Clipboard", Json::str(
        c.osc52Clipboard == Osc52Policy::Allow ? "allow" :
        c.osc52Clipboard == Osc52Policy::Deny ? "deny" : "ask"));
    Json hooks = Json::object();
    hooks.set("sessionStart", Json::str(wideToUtf8(c.sessionStartHook)));
    hooks.set("sessionExit", Json::str(wideToUtf8(c.sessionExitHook)));
    hooks.set("appExit", Json::str(wideToUtf8(c.appExitHook)));
    j.set("hooks", std::move(hooks));
    Json hosts = Json::array();
    for (const auto& h : c.sshHosts) {
        Json item = Json::object();
        item.set("name", Json::str(wideToUtf8(h.name)));
        item.set("host", Json::str(wideToUtf8(h.host)));
        item.set("user", Json::str(wideToUtf8(h.user)));
        item.set("port", Json::number(h.port));
        item.set("identityFile", Json::str(wideToUtf8(h.identityFile)));
        hosts.push(std::move(item));
    }
    j.set("sshHosts", std::move(hosts));
    Json serialPorts = Json::array();
    for (const auto& p : c.serialPorts) {
        Json item = Json::object();
        item.set("name", Json::str(wideToUtf8(p.name)));
        item.set("port", Json::str(wideToUtf8(p.port)));
        item.set("baudRate", Json::number(p.baudRate));
        item.set("dataBits", Json::number(p.dataBits));
        item.set("parity", Json::str(serialParityName(p.parity)));
        item.set("stopBits", Json::str(serialStopBitsName(p.stopBits)));
        item.set("mode", Json::str(serialModeName(p.mode)));
        item.set("lineEnding", Json::str(serialLineEndingName(p.lineEnding)));
        serialPorts.push(std::move(item));
    }
    j.set("serialPorts", std::move(serialPorts));
    Json agents = Json::array();
    for (const auto& a : c.agents) {
        Json item = Json::object();
        item.set("name", Json::str(wideToUtf8(a.name)));
        item.set("command", Json::str(wideToUtf8(a.command)));
        item.set("cwd", Json::str(wideToUtf8(a.cwd)));
        item.set("testCommand", Json::str(wideToUtf8(a.testCommand)));
        agents.push(std::move(item));
    }
    j.set("agents", std::move(agents));
    Json bindings = Json::object();
    for (const auto& binding : c.keybindings)
        bindings.set(wideToUtf8(binding.action),
                     Json::str(wideToUtf8(formatKeyChord(binding.chord))));
    j.set("keybindings", std::move(bindings));
    Json projects = Json::array();
    for (const auto& project : c.projects)
        projects.push(Json::str(wideToUtf8(project)));
    j.set("projects", std::move(projects));
    Json exclusions = Json::array();
    for (const auto& project : c.workspaceExclusions)
        exclusions.push(Json::str(wideToUtf8(project)));
    j.set("workspaceExclusions", std::move(exclusions));
    Json recent = Json::array();
    for (const auto& project : c.recentProjects)
        recent.push(Json::str(wideToUtf8(project)));
    j.set("recentProjects", std::move(recent));
    Json favorites = Json::array();
    for (const auto& project : c.favoriteProjects)
        favorites.push(Json::str(wideToUtf8(project)));
    j.set("favoriteProjects", std::move(favorites));
    j.set("settingsPage", Json::number(c.settingsPage));
    Json icons = Json::object();
    for (const auto& icon : c.projectIcons)
        icons.set(wideToUtf8(icon.first), Json::str(wideToUtf8(icon.second)));
    j.set("projectIcons", std::move(icons));
    Json colors = Json::object();
    for (const auto& color : c.projectColors)
        colors.set(wideToUtf8(color.first), Json::str(colorToHex(color.second)));
    j.set("projectColors", std::move(colors));
    Json archived = Json::array();
    for (const auto& project : c.archivedProjects)
        archived.push(Json::str(wideToUtf8(project)));
    j.set("archivedProjects", std::move(archived));
    if (!c.themeName.empty())
        j.set("theme", Json::str(wideToUtf8(c.themeName)));
    return j.dump(2);
}

} // namespace

std::wstring configDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"LINEY_CONFIG_DIR", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        CreateDirectoryW(buf, nullptr);
        return buf;
    }
    n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buf) + L"\\.liney";
    CreateDirectoryW(dir.c_str(), nullptr);  // ignore "already exists"
    return dir;
}

Config loadConfig() {
    Config cfg;
    // Seed the palette from the first built-in preset so an unset/partial
    // theme still yields a coherent look (terminal + chrome together).
    const std::vector<ThemePreset> presets = builtinThemePresets();
    if (!presets.empty()) {
        cfg.themeName = presets[0].name;
        cfg.theme = presets[0].terminal;
        cfg.uiTheme = presets[0].ui;
    }
    const std::wstring dir = configDir();
    if (dir.empty()) return cfg;
    const std::wstring path = dir + L"\\config.json";

    const std::string text = readFile(path);
    if (text.empty()) {
        cfg.firstRun = true;
        writeFile(path, defaultJson(cfg));  // first run: seed a default
        return cfg;
    }

    bool ok = false;
    Json j = Json::parse(text, &ok);
    if (!ok || !j.isObject()) {
        const std::wstring backupPath = path + L".bak";
        const std::string backup = readFile(backupPath);
        bool backupOk = false;
        Json recovered = backup.empty() ? Json() : Json::parse(backup, &backupOk);
        const std::wstring brokenPath = path + L".invalid";
        DeleteFileW(brokenPath.c_str());
        MoveFileExW(path.c_str(), brokenPath.c_str(), MOVEFILE_REPLACE_EXISTING);
        if (backupOk && recovered.isObject()) {
            writeFileAtomic(path, backup);
            j = std::move(recovered);
            configWarning(
                L"config.json was restored from config.json.bak.\n\n"
                L"The invalid file is preserved at:\n" + brokenPath,
                L"Liney - configuration recovered");
        } else {
            writeFileAtomic(path, defaultJson(cfg));
            configWarning(
                L"Liney started with safe defaults.\n\nThe invalid file is "
                L"preserved at:\n" + brokenPath,
                L"Liney - configuration recovered");
            return cfg;
        }
    }

    int schemaVersion = kConfigSchemaVersion;
    if (jsonInteger(j["schemaVersion"], 1, std::numeric_limits<int>::max(),
                    schemaVersion))
        cfg.schemaVersion = schemaVersion;
    if (cfg.schemaVersion < 1) cfg.schemaVersion = 1;
    if (cfg.schemaVersion > kConfigSchemaVersion) {
        configWarning(
            L"This config.json was written by a newer Liney version. Known "
            L"settings will be used, but Liney will not rewrite it.",
            L"Liney - newer configuration");
    }

    if (j.contains("shell")) cfg.shell = utf8ToWide(j["shell"].asString());
    if (j.contains("fontFamily"))
        cfg.fontFamily = utf8ToWide(j["fontFamily"].asString());
    if (j.contains("fontSize"))
        jsonFloat(j["fontSize"], 6.0f, 96.0f, cfg.fontSize);
    if (j.contains("fontLigatures"))
        cfg.fontLigatures = j["fontLigatures"].asBool(cfg.fontLigatures);
    if (j.contains("scrollback"))
        jsonInteger(j["scrollback"], 0, 1000000, cfg.scrollback);
    if (j.contains("workspaceRoot"))
        cfg.workspaceRoot = utf8ToWide(j["workspaceRoot"].asString());
    // hooks.{sessionStart,sessionExit,appExit}
    cfg.sessionStartHook = utf8ToWide(j["hooks"]["sessionStart"].asString());
    cfg.sessionExitHook = utf8ToWide(j["hooks"]["sessionExit"].asString());
    cfg.appExitHook = utf8ToWide(j["hooks"]["appExit"].asString());
    // sshHosts accepts the old ["user@host", ...] form as well as the
    // separate {"user": ..., "host": ...} form.
    if (j["sshHosts"].isArray()) {
        for (const Json& host : j["sshHosts"].items()) {
            if (host.type() == Json::Type::String) {
                const std::wstring value = utf8ToWide(host.asString());
                SshProfile profile;
                if (splitLegacySshTarget(value, profile.user, profile.host)) {
                    profile.name = value;
                    cfg.sshHosts.push_back(std::move(profile));
                }
            } else if (host.isObject()) {
                SshProfile profile;
                profile.host = utf8ToWide(host["host"].asString());
                if (host.contains("user")) {
                    profile.user = utf8ToWide(host["user"].asString());
                } else {
                    std::wstring legacyHost;
                    std::wstring legacyUser;
                    if (!splitLegacySshTarget(profile.host, legacyUser,
                                              legacyHost))
                        continue;
                    profile.user = std::move(legacyUser);
                    profile.host = std::move(legacyHost);
                }
                profile.name = utf8ToWide(host["name"].asString());
                int port = 22;
                if (host.contains("port") &&
                    !jsonInteger(host["port"], 1, 65535, port))
                    continue;
                profile.port = port;
                profile.identityFile = utf8ToWide(host["identityFile"].asString());
                if (profile.name.empty()) profile.name = sshProfileTarget(profile);
                if (validSshHost(profile.host) && validSshUser(profile.user) &&
                    profile.port >= 1 &&
                    profile.port <= 65535) cfg.sshHosts.push_back(std::move(profile));
            }
        }
    }
    if (j["serialPorts"].isArray()) {
        for (const Json& item : j["serialPorts"].items()) {
            if (!item.isObject() || cfg.serialPorts.size() >= 128) continue;
            SerialProfile profile;
            profile.name = utf8ToWide(item["name"].asString());
            profile.port = utf8ToWide(item["port"].asString());
            uint32_t number = 0;
            if (item.contains("baudRate") &&
                !jsonUnsigned(item["baudRate"], 4'000'000, number))
                continue;
            if (item.contains("baudRate")) profile.baudRate = number;
            if (item.contains("dataBits")) {
                if (!jsonUnsigned(item["dataBits"], 8, number) || number < 5)
                    continue;
                profile.dataBits = static_cast<uint8_t>(number);
            }
            if (item.contains("parity")) {
                if (item["parity"].type() != Json::Type::String ||
                    !parseSerialParity(item["parity"].asString(), profile.parity))
                    continue;
            }
            if (item.contains("stopBits")) {
                if (item["stopBits"].type() != Json::Type::String ||
                    !parseSerialStopBits(item["stopBits"].asString(), profile.stopBits))
                    continue;
            }
            if (item.contains("mode")) {
                if (item["mode"].type() != Json::Type::String ||
                    !parseSerialMode(item["mode"].asString(), profile.mode))
                    continue;
            }
            if (item.contains("lineEnding")) {
                if (item["lineEnding"].type() != Json::Type::String ||
                    !parseSerialLineEnding(item["lineEnding"].asString(),
                                           profile.lineEnding))
                    continue;
            }
            if (validSerialProfile(profile))
                cfg.serialPorts.push_back(std::move(profile));
        }
    }
    // agents: [{ name, command, cwd }]
    if (j["agents"].isArray())
        for (const Json& a : j["agents"].items())
            if (a.isObject()) {
                AgentDef d;
                d.name = utf8ToWide(a["name"].asString());
                d.command = utf8ToWide(a["command"].asString());
                d.cwd = utf8ToWide(a["cwd"].asString());
                d.testCommand = utf8ToWide(a["testCommand"].asString());
                if (!d.command.empty()) {
                    if (d.name.empty()) d.name = d.command;
                    cfg.agents.push_back(d);
                }
            }
    const Json& keybindings = j["keybindings"];
    if (keybindings.isObject()) {
        bool conflict = false;
        for (const auto& item : keybindings.members()) {
            KeyChord chord;
            if (parseKeyChord(utf8ToWide(item.second.asString()), chord)) {
                bool duplicate = false;
                for (const auto& existing : cfg.keybindings)
                    if (sameKeyChord(existing.chord, chord)) {
                        duplicate = true;
                        conflict = true;
                        break;
                    }
                if (!duplicate)
                    cfg.keybindings.push_back({utf8ToWide(item.first), chord});
            }
        }
        if (conflict)
            configWarning(L"Two custom actions use the same shortcut. The first binding was kept; resolve the duplicate in config.json.",
                          L"Liney - shortcut conflict");
    }
    // theme: either a preset NAME (string, e.g. "Azure Night") that picks a
    // coordinated terminal + chrome look, or the legacy { background,
    // foreground, palette } OBJECT of terminal-only overrides.
    const Json& t = j["theme"];
    if (t.type() == Json::Type::String) {
        if (const ThemePreset* p =
                findThemePreset(presets, utf8ToWide(t.asString()))) {
            cfg.themeName = p->name;
            cfg.theme = p->terminal;
            cfg.uiTheme = p->ui;
        }
    } else if (t.isObject()) {
        cfg.theme.background =
            hexToColor(t["background"].asString(), cfg.theme.background);
        cfg.theme.foreground =
            hexToColor(t["foreground"].asString(), cfg.theme.foreground);
        const Json& pal = t["palette"];
        if (pal.isArray())
            for (int k = 0; k < 16 && k < static_cast<int>(pal.size()); ++k)
                cfg.theme.ansi[k] =
                    hexToColor(pal.items()[k].asString(), cfg.theme.ansi[k]);
    }
    // accentColor: "#RRGGBB" override for the chrome accent (active-pane
    // divider / active tab / icons) on top of whatever preset is active.
    if (j.contains("accentColor")) {
        cfg.uiTheme.accent =
            hexToColor(j["accentColor"].asString(), cfg.uiTheme.accent);
    }

    if (j.contains("unixTools")) cfg.unixTools = j["unixTools"].asBool(true);
    if (j.contains("copyOnSelect"))
        cfg.copyOnSelect = j["copyOnSelect"].asBool(false);
    if (j.contains("multiLinePasteWarning"))
        cfg.multiLinePasteWarning = j["multiLinePasteWarning"].asBool(true);
    if (j.contains("rememberLayout"))
        cfg.rememberLayout = j["rememberLayout"].asBool(false);
    if (j.contains("rememberPanelLayout"))
        cfg.rememberPanelLayout = j["rememberPanelLayout"].asBool(true);
    if (j.contains("sidebarVisible"))
        cfg.sidebarVisible = j["sidebarVisible"].asBool(true);
    if (j.contains("filesPanelVisible"))
        cfg.filesPanelVisible = j["filesPanelVisible"].asBool(false);
    if (j.contains("sidebarWidth"))
        jsonFloat(j["sidebarWidth"], 144.0f, 640.0f, cfg.sidebarWidth);
    if (j.contains("filesPanelWidth"))
        jsonFloat(j["filesPanelWidth"], 144.0f, 640.0f,
                  cfg.filesPanelWidth);
    if (j.contains("splitUseWorkspaceDir"))
        cfg.splitUseWorkspaceDir = j["splitUseWorkspaceDir"].asBool(false);
    if (j.contains("powerShellHistoryPerProject"))
        cfg.powerShellHistoryPerProject =
            j["powerShellHistoryPerProject"].asBool(false);
    if (j.contains("checkForUpdatesOnStartup"))
        cfg.checkForUpdatesOnStartup = j["checkForUpdatesOnStartup"].asBool(true);
    if (j["ai"].isObject()) {
        cfg.aiProvider = utf8ToWide(j["ai"]["provider"].asString("off"));
        cfg.aiModel = utf8ToWide(j["ai"]["model"].asString("gpt-5.6-luna"));
        cfg.aiEndpoint = utf8ToWide(j["ai"]["endpoint"].asString(
            "https://api.openai.com/v1/responses"));
        cfg.aiIncludeCwd = j["ai"]["includeCwd"].asBool(false);
    }
    if (cfg.aiProvider != L"openai" && cfg.aiProvider != L"codex" &&
        cfg.aiProvider != L"custom")
        cfg.aiProvider = L"off";
    if (cfg.aiModel.empty()) cfg.aiModel = L"gpt-5.6-luna";
    if (j.contains("osc52Clipboard")) {
        const std::string policy = j["osc52Clipboard"].asString();
        cfg.osc52Clipboard = policy == "allow" ? Osc52Policy::Allow :
                             policy == "deny" ? Osc52Policy::Deny :
                                                  Osc52Policy::Ask;
    }
    // projectIcons: { "<repoName>": "builtin:<id>" }
    const Json& pi = j["projectIcons"];
    if (pi.isObject())
        for (const auto& kv : pi.members())
            cfg.projectIcons.push_back(
                { utf8ToWide(kv.first), utf8ToWide(kv.second.asString()) });
    // projectColors: { "C:/path/to/project": "#RRGGBB" }
    const Json& pc = j["projectColors"];
    if (pc.isObject())
        for (const auto& kv : pc.members())
            if (!kv.first.empty() && kv.second.type() == Json::Type::String)
                cfg.projectColors.push_back(
                    { utf8ToWide(kv.first),
                      hexToColor(kv.second.asString(), Color{120, 200, 160}) });
    if (j["archivedProjects"].isArray())
        for (const Json& p : j["archivedProjects"].items())
            if (p.type() == Json::Type::String && !p.asString().empty())
                cfg.archivedProjects.push_back(utf8ToWide(p.asString()));
    // projects: ["C:/path/to/folder", ...] (explicit sidebar projects)
    if (j["projects"].isArray())
        for (const Json& p : j["projects"].items())
            if (p.type() == Json::Type::String && !p.asString().empty())
                cfg.projects.push_back(utf8ToWide(p.asString()));
    if (j["workspaceExclusions"].isArray())
        for (const Json& p : j["workspaceExclusions"].items())
            if (p.type() == Json::Type::String && !p.asString().empty())
                cfg.workspaceExclusions.push_back(
                    utf8ToWide(p.asString()));
    if (j["recentProjects"].isArray())
        for (const Json& p : j["recentProjects"].items())
            if (p.type() == Json::Type::String && !p.asString().empty() &&
                cfg.recentProjects.size() < 10)
                cfg.recentProjects.push_back(utf8ToWide(p.asString()));
    if (j["favoriteProjects"].isArray())
        for (const Json& p : j["favoriteProjects"].items())
            if (p.type() == Json::Type::String && !p.asString().empty())
                cfg.favoriteProjects.push_back(utf8ToWide(p.asString()));
    int settingsPage = 0;
    if (jsonInteger(j["settingsPage"], 0, 3, settingsPage))
        cfg.settingsPage = settingsPage;

    if (cfg.shell.empty()) cfg.shell = L"cmd.exe";
    if (cfg.fontFamily.empty()) cfg.fontFamily = L"Cascadia Mono";
    if (cfg.fontSize < 6.0f || cfg.fontSize > 96.0f) cfg.fontSize = 16.0f;
    if (cfg.scrollback < 0) cfg.scrollback = 0;
    if (cfg.scrollback > 1000000) cfg.scrollback = 1000000;  // sane upper bound
    if (!std::isfinite(cfg.sidebarWidth) || cfg.sidebarWidth < 144.0f ||
        cfg.sidebarWidth > 640.0f)
        cfg.sidebarWidth = 224.0f;
    if (!std::isfinite(cfg.filesPanelWidth) || cfg.filesPanelWidth < 144.0f ||
        cfg.filesPanelWidth > 640.0f)
        cfg.filesPanelWidth = 224.0f;
    return cfg;
}

namespace {
// Re-parse config.json, apply `mutate`, and write it back so every other key
// survives the rewrite; the Json type preserves object key order, so the file
// stays stable.
template <class Fn>
bool updateConfigFile(Fn mutate) {
    const std::wstring dir = configDir();
    if (dir.empty()) return false;
    const std::wstring path = dir + L"\\config.json";
    const std::string text = readFile(path);
    Json j = Json::object();
    if (!text.empty()) {
        bool ok = false;
        j = Json::parse(text, &ok);
        // The file exists but doesn't parse (hand-edit typo, corruption).
        // Skip the save: dropping one setting update is recoverable, silently
        // rewriting the file with a near-empty object is not.
        if (!ok || !j.isObject()) return false;
        if (j["schemaVersion"].asNumber(kConfigSchemaVersion) >
            kConfigSchemaVersion)
            return false;
    }
    mutate(j);
    j.set("schemaVersion", Json::number(kConfigSchemaVersion));
    return writeFileAtomicWithBackup(path, j.dump(2));
}
} // namespace

void saveFontSize(float size) {
    updateConfigFile([&](Json& j) { j.set("fontSize", Json::number(size)); });
}

void saveFontFamily(const std::wstring& family) {
    updateConfigFile(
        [&](Json& j) { j.set("fontFamily", Json::str(wideToUtf8(family))); });
}

// Write via a temp file + atomic rename so a crash/power loss mid-write can't
// leave a truncated JSON file behind.
bool writeFileAtomic(const std::wstring& path, const std::string& content) {
    const std::wstring tmp = path + L".tmp";
    bool writeOk = false;
    {
        std::ofstream f(tmp.c_str(), std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(content.data(), static_cast<std::streamsize>(content.size()));
            f.flush();
            writeOk = f.good();
        }
    }
    if (!writeOk) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool writeFileAtomicWithBackup(const std::wstring& path,
                               const std::string& content) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        const std::wstring backup = path + L".bak";
        if (!CopyFileW(path.c_str(), backup.c_str(), FALSE)) return false;
    }
    return writeFileAtomic(path, content);
}

bool updateConfigJson(const std::function<void(Json&)>& mutate) {
    return updateConfigFile([&](Json& j) { mutate(j); });
}

bool saveSshProfile(const SshProfile& profile) {
    if (!validSshProfile(profile)) return false;
    return updateConfigJson([&](Json& root) {
        Json hosts = root["sshHosts"].isArray()
                         ? root["sshHosts"]
                         : Json::array();
        Json item = Json::object();
        item.set("name", Json::str(wideToUtf8(profile.name)));
        item.set("host", Json::str(wideToUtf8(profile.host)));
        item.set("user", Json::str(wideToUtf8(profile.user)));
        item.set("port", Json::number(profile.port));
        item.set("identityFile", Json::str(wideToUtf8(profile.identityFile)));
        hosts.push(std::move(item));
        root.set("sshHosts", std::move(hosts));
    });
}

bool saveSshProfiles(const std::vector<SshProfile>& profiles) {
    for (const SshProfile& profile : profiles)
        if (!validSshProfile(profile)) return false;
    return updateConfigJson([&](Json& root) {
        Json hosts = Json::array();
        for (const SshProfile& profile : profiles) {
            Json item = Json::object();
            item.set("name", Json::str(wideToUtf8(profile.name)));
            item.set("host", Json::str(wideToUtf8(profile.host)));
            item.set("user", Json::str(wideToUtf8(profile.user)));
            item.set("port", Json::number(profile.port));
            item.set("identityFile", Json::str(wideToUtf8(profile.identityFile)));
            hosts.push(std::move(item));
        }
        root.set("sshHosts", std::move(hosts));
    });
}

bool saveSerialProfile(const SerialProfile& profile) {
    if (!validSerialProfile(profile)) return false;
    return updateConfigJson([&](Json& root) {
        Json ports = root["serialPorts"].isArray()
                         ? root["serialPorts"]
                         : Json::array();
        Json item = Json::object();
        item.set("name", Json::str(wideToUtf8(profile.name)));
        item.set("port", Json::str(wideToUtf8(profile.port)));
        item.set("baudRate", Json::number(profile.baudRate));
        item.set("dataBits", Json::number(profile.dataBits));
        item.set("parity", Json::str(serialParityName(profile.parity)));
        item.set("stopBits", Json::str(serialStopBitsName(profile.stopBits)));
        item.set("mode", Json::str(serialModeName(profile.mode)));
        item.set("lineEnding", Json::str(serialLineEndingName(profile.lineEnding)));
        ports.push(std::move(item));
        root.set("serialPorts", std::move(ports));
    });
}

bool saveSerialProfiles(const std::vector<SerialProfile>& profiles) {
    for (const SerialProfile& profile : profiles)
        if (!validSerialProfile(profile)) return false;
    return updateConfigJson([&](Json& root) {
        Json ports = Json::array();
        for (const SerialProfile& profile : profiles) {
            Json item = Json::object();
            item.set("name", Json::str(wideToUtf8(profile.name)));
            item.set("port", Json::str(wideToUtf8(profile.port)));
            item.set("baudRate", Json::number(profile.baudRate));
            item.set("dataBits", Json::number(profile.dataBits));
            item.set("parity", Json::str(serialParityName(profile.parity)));
            item.set("stopBits", Json::str(serialStopBitsName(profile.stopBits)));
            item.set("mode", Json::str(serialModeName(profile.mode)));
            item.set("lineEnding",
                     Json::str(serialLineEndingName(profile.lineEnding)));
            ports.push(std::move(item));
        }
        root.set("serialPorts", std::move(ports));
    });
}

} // namespace liney

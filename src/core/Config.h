#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Themes.h"
#include "render/Cell.h"
#include "util/Json.h"
#include "core/KeyBinding.h"
#include "core/SshProfiles.h"
#include "core/SerialProfiles.h"

namespace liney {

// An agent-backed session: a named entry that opens a tab running `command`
// (e.g. an AI agent CLI) in `cwd`. Mirrors macOS liney's agent sessions.
struct AgentDef {
    std::wstring name;
    std::wstring command;
    std::wstring cwd;  // empty => inherit
    std::wstring testCommand; // optional verification command for review
};

enum class Osc52Policy { Deny, Ask, Allow };

// User settings, loaded from %USERPROFILE%\.liney\config.json. A default file is
// written on first run. Mirrors macOS liney's ~/.liney/ persistence directory.
struct Config {
    bool firstRun = false;                    // ephemeral; config was created now
    int schemaVersion = 1;
    std::wstring shell = L"cmd.exe";        // default shell for new tabs
    std::wstring fontFamily = L"Cascadia Mono";
    float fontSize = 16.0f;
    bool fontLigatures = false;             // opt-in programming ligatures
    int scrollback = 10000;                 // history lines retained per session
    std::wstring workspaceRoot;             // empty => explicit projects only
    std::wstring sessionStartHook;          // command run in each new shell
    std::wstring sessionExitHook;           // command run when a pane closes
    std::wstring appExitHook;               // command run on app quit
    std::vector<SshProfile> sshHosts;
    std::vector<SerialProfile> serialPorts;
    std::vector<AgentDef> agents;           // sidebar AGENTS list
    std::vector<KeyBinding> keybindings;
    std::wstring themeName;                  // active preset name (see Themes.h)
    Theme theme;                            // terminal palette (preset + overrides)
    UiTheme uiTheme;                        // chrome palette (preset + accent override)
    bool unixTools = true;                  // append Git's usr/bin to PATH (ls/cat/…)
    bool copyOnSelect = false;              // copy to clipboard as soon as a drag ends
    bool multiLinePasteWarning = true;      // confirm before pasting multiple lines
    bool rememberLayout = false;            // restore tabs/panes on launch (off by default)
    bool rememberPanelLayout = true;        // restore side-panel visibility + widths
    bool sidebarVisible = true;             // last workspace sidebar state
    bool filesPanelVisible = false;         // last file navigator state
    float sidebarWidth = 224.0f;            // logical pixels, before DPI scaling
    float filesPanelWidth = 224.0f;         // logical pixels, before DPI scaling
    bool splitUseWorkspaceDir = false;      // new splits open in workspace/home dir (else inherit the pane's cwd)
    bool powerShellHistoryPerProject = false; // opt-in PSReadLine history per project/worktree
    bool checkForUpdatesOnStartup = true;  // quiet GitHub release check after launch
    std::wstring aiProvider = L"off";      // off | openai | codex | custom
    std::wstring aiModel = L"gpt-5.6-luna";
    std::wstring aiEndpoint = L"https://api.openai.com/v1/responses";
    bool aiIncludeCwd = false;              // explicit privacy opt-in
    Osc52Policy osc52Clipboard = Osc52Policy::Ask;
    // Per-project sidebar icons: repo name -> "builtin:<id>".
    // Legacy png/ico paths remain readable for backwards compatibility.
    std::vector<std::pair<std::wstring, std::wstring>> projectIcons;
    // Per-project chrome colors, keyed by normalized project path.
    std::vector<std::pair<std::wstring, Color>> projectColors;
    // Projects hidden from the active workspace list but still restorable.
    std::vector<std::wstring> archivedProjects;
    // Explicit project folders added to the sidebar (besides scanned ones).
    std::vector<std::wstring> projects;
    // Auto-discovered repositories explicitly removed from the sidebar.
    std::vector<std::wstring> workspaceExclusions;
    std::vector<std::wstring> recentProjects;
    std::vector<std::wstring> favoriteProjects;
    int settingsPage = 0;  // last selected Settings category (0..3)
};

// %USERPROFILE%\.liney (created if missing). Empty if USERPROFILE is unset.
std::wstring configDir();

// Load config (creating the directory + a default config.json if absent).
Config loadConfig();

// Persist just the fontSize back to config.json, preserving every other key
// (parse → set → dump). Best-effort: silently no-ops if the file can't be
// read/written. Used to remember the zoom level across launches.
void saveFontSize(float size);

// Persist just the fontFamily, same parse → set → dump approach as
// saveFontSize. Used by the in-app font picker.
void saveFontFamily(const std::wstring& family);

// Write `content` via temp file + atomic rename, so a crash mid-write can't
// leave a truncated file. Returns false on failure (target left untouched).
bool writeFileAtomic(const std::wstring& path, const std::string& content);

// Preserve the last known-good target as `<path>.bak`, then atomically write.
bool writeFileAtomicWithBackup(const std::wstring& path,
                               const std::string& content);

// Re-parse config.json, apply `mutate`, write it back atomically. Preserves
// every other key; refuses to overwrite a config.json that no longer parses
// (so a hand-edit typo can't cost the user their whole file). Returns false
// when the config directory/path is unavailable, the existing JSON is invalid,
// or the atomic write fails.
bool updateConfigJson(const std::function<void(Json&)>& mutate);

// Append one validated profile to sshHosts while preserving every other
// config key. Legacy string entries are retained alongside the new object
// format. Returns false when validation or persistence fails.
bool saveSshProfile(const SshProfile& profile);
bool saveSshProfiles(const std::vector<SshProfile>& profiles);

// Append one validated serial profile to serialPorts while preserving every
// other config key. Returns false when validation or persistence fails.
bool saveSerialProfile(const SerialProfile& profile);
bool saveSerialProfiles(const std::vector<SerialProfile>& profiles);

} // namespace liney

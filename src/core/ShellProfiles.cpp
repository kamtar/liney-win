#include "core/ShellProfiles.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "core/Config.h"

namespace liney {
namespace {

std::wstring findOnPath(const wchar_t* executable) {
    wchar_t path[32768]{};
    DWORD n = SearchPathW(nullptr, executable, nullptr,
                          static_cast<DWORD>(_countof(path)), path, nullptr);
    return n > 0 && n < _countof(path) ? std::wstring(path) : std::wstring();
}

bool fileExists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

void add(std::vector<ShellProfile>& out, const wchar_t* id, const wchar_t* name,
         const std::wstring& command) {
    if (command.empty()) return;
    for (const auto& profile : out)
        if (_wcsicmp(profile.command.c_str(), command.c_str()) == 0) return;
    out.push_back({id, name, command});
}

std::wstring quotePowerShellString(const std::wstring& value) {
    std::wstring quoted = value;
    size_t quote = 0;
    while ((quote = quoted.find(L'\'', quote)) != std::wstring::npos) {
        quoted.insert(quote, 1, L'\'');
        quote += 2;
    }
    return L"'" + quoted + L"'";
}

std::wstring historyBootstrap(const std::wstring& historyPath,
                              const std::wstring& scriptPath) {
    const std::wstring source = L". " + quotePowerShellString(scriptPath);
    if (historyPath.empty()) return source;
    return L"$env:LINEY_PSREADLINE_HISTORY_PATH = " +
           quotePowerShellString(historyPath) + L"; " + source;
}

std::wstring addHistoryToPreparedCommand(const std::wstring& command,
                                         const std::wstring& historyPath) {
    if (historyPath.empty()) return command;
    std::wstring lower = command;
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    // Layouts written by an older Liney may already contain the integration
    // command. Reuse them, but inject the new per-session setting into the
    // existing -Command payload. New commands are generated with the marker
    // below and are already idempotent.
    if (lower.find(L"liney_psreadline_history_path") != std::wstring::npos)
        return command;
    const size_t option = lower.find(L"-command");
    if (option == std::wstring::npos) return command;
    const size_t openingQuote = command.find(L'\"', option);
    if (openingQuote == std::wstring::npos) return command;
    const std::wstring assignment =
        L"$env:LINEY_PSREADLINE_HISTORY_PATH = " +
        quotePowerShellString(historyPath) + L"; ";
    return command.substr(0, openingQuote + 1) + assignment +
           command.substr(openingQuote + 1);
}

} // namespace

std::vector<ShellProfile> discoverShellProfiles() {
    std::vector<ShellProfile> out;
    std::wstring path = findOnPath(L"pwsh.exe");
    if (!path.empty()) add(out, L"pwsh", L"PowerShell 7", L"\"" + path + L"\"");

    path = findOnPath(L"powershell.exe");
    if (!path.empty())
        add(out, L"windows-powershell", L"Windows PowerShell", L"\"" + path + L"\"");

    path = findOnPath(L"wsl.exe");
    if (!path.empty()) add(out, L"wsl", L"WSL", L"\"" + path + L"\"");

    wchar_t programFiles[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH)) {
        const std::wstring gitBash =
            std::wstring(programFiles) + L"\\Git\\bin\\bash.exe";
        if (fileExists(gitBash))
            add(out, L"git-bash", L"Git Bash",
                L"\"" + gitBash + L"\" --login -i");
    }

    path = findOnPath(L"cmd.exe");
    add(out, L"cmd", L"Command Prompt",
        path.empty() ? L"cmd.exe" : L"\"" + path + L"\"");
    return out;
}

std::wstring powerShellHistoryPath(const std::wstring& projectPath,
                                   const std::wstring& worktreePath) {
    const std::wstring fileName =
        powerShellHistoryFileName(projectPath, worktreePath);
    if (fileName.empty()) return L"";
    const std::wstring dir = configDir();
    if (dir.empty()) return L"";
    const std::wstring historyDir = dir + L"\\powershell-history";
    CreateDirectoryW(historyDir.c_str(), nullptr);
    return historyDir + L"\\" + fileName;
}

std::wstring prepareShellCommand(const std::wstring& command,
                                 const std::wstring& historyPath) {
    std::wstring lower = command;
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    const bool powershell = lower.find(L"pwsh.exe") != std::wstring::npos ||
                            lower.find(L"powershell.exe") != std::wstring::npos;
    if (!powershell) return command;
    if (lower.find(L"liney-shell-integration.ps1") != std::wstring::npos)
        return addHistoryToPreparedCommand(command, historyPath);

    const std::wstring dir = configDir();
    if (dir.empty()) return command;
    const std::wstring path = dir + L"\\liney-shell-integration.ps1";
    static const char script[] = R"PS1(# Liney PowerShell integration: OSC 7 cwd + OSC 133 command semantics.
if ($env:LINEY_SHELL_INTEGRATION_ACTIVE) { return }
$env:LINEY_SHELL_INTEGRATION_ACTIVE = '1'
$script:LineyEsc = [char]27
$script:LineyOriginalPrompt = $function:prompt
function script:Install-LineyReadLineOptions {
    if (-not (Get-Module -Name PSReadLine)) { return }
    if ($env:LINEY_PSREADLINE_HISTORY_PATH -and
        -not $script:LineyHistoryPathInstalled) {
        try {
            Set-PSReadLineOption -HistorySavePath $env:LINEY_PSREADLINE_HISTORY_PATH
            $script:LineyHistoryPathInstalled = $true
        } catch {
            # Older PSReadLine builds may not expose HistorySavePath. Keep the
            # normal global history rather than breaking the prompt.
        }
    }
    if ($script:LineyEnterHandlerInstalled) { return }
    Set-PSReadLineKeyHandler -Key Enter -ScriptBlock {
        [Console]::Write("$script:LineyEsc]133;C$script:LineyEsc\")
        [Microsoft.PowerShell.PSConsoleReadLine]::AcceptLine()
    }
    $script:LineyEnterHandlerInstalled = $true
}
function global:prompt {
    # PSReadLine is commonly imported after a -Command bootstrap script runs.
    # Install the handler lazily from the first prompt so command-start OSC 133
    # markers are present on both Windows PowerShell and PowerShell 7.
    Install-LineyReadLineOptions
    $code = if ($null -eq $global:LASTEXITCODE) { 0 } else { $global:LASTEXITCODE }
    $cwd = (Get-Location).Path.Replace('\', '/')
    [Console]::Write("$script:LineyEsc]133;D;$code$script:LineyEsc\")
    [Console]::Write("$script:LineyEsc]7;file://localhost/$cwd$script:LineyEsc\")
    [Console]::Write("$script:LineyEsc]133;A$script:LineyEsc\")
    $text = if ($script:LineyOriginalPrompt) { & $script:LineyOriginalPrompt } else { "PS $cwd> " }
    return "$text$script:LineyEsc]133;B$script:LineyEsc\"
}
try { Import-Module PSReadLine -ErrorAction Stop } catch { }
Install-LineyReadLineOptions
)PS1";
    if (!writeFileAtomic(path, script)) return command;
    // -NoExit keeps the profile interactive after the bootstrap command.
    return command + L" -NoLogo -ExecutionPolicy Bypass -NoExit -Command \"" +
           historyBootstrap(historyPath, path) + L"\"";
}

} // namespace liney

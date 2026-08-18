@echo off
setlocal EnableExtensions

rem Build both release artifacts from the repository root:
rem   dist\liney-setup.exe
rem   dist\liney-portable.zip

cd /d "%~dp0"
set "ROOT=%~dp0"
set "VSDEV="

rem If this is not already a Visual Studio developer shell, initialize the
rem first installed VS 2022/18 x64 toolchain we can find.
where cl.exe >nul 2>&1
if not errorlevel 1 goto toolchain_ready

for %%P in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\18\Professional\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\Common7\Tools\VsDevCmd.bat"
  "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat"
) do (
  if not defined VSDEV if exist "%%~P" set "VSDEV=%%~P"
)

if not defined VSDEV (
  echo ERROR: Visual Studio 2022/18 x64 developer tools were not found.
  echo Run this file from an x64 Native Tools Command Prompt for VS 2022.
  exit /b 1
)

call "%VSDEV%" -arch=x64
if errorlevel 1 (
  echo ERROR: Visual Studio developer environment could not be initialized.
  exit /b 1
)

:toolchain_ready
where cmake.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: CMake was not found on PATH.
  exit /b 1
)

rem Prefer the repository's pinned Zig when it exists; otherwise require Zig on PATH.
if exist "%ROOT%tools\zig\zig-x86_64-windows-0.15.2\zig.exe" (
  set "PATH=%ROOT%tools\zig\zig-x86_64-windows-0.15.2;%PATH%"
)
where zig.exe >nul 2>&1
if errorlevel 1 (
  echo ERROR: Zig 0.15.2 was not found on PATH or under tools\zig.
  exit /b 1
)

echo Building release installer and portable package...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%ROOT%tools\make-installer.ps1" -BuildDir "%ROOT%build-ghostty"
if errorlevel 1 (
  echo ERROR: Packaging failed.
  exit /b 1
)

echo.
echo Packaging complete:
echo   %ROOT%dist\liney-setup.exe
echo   %ROOT%dist\liney-portable.zip
exit /b 0

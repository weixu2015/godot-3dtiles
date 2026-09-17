@echo off
REM ---------------------------------------------------------------------------
REM Activates the MSVC x64 toolchain environment.
REM
REM This project uses the Ninja generator, which (unlike the Visual Studio
REM generator) does not locate cl.exe on its own. Ninja needs cl.exe, link.exe and
REM the Windows SDK on PATH, so this script must be called before every cmake
REM configure/build on Windows.
REM
REM vswhere is used to find any installation that actually ships the C++ toolset,
REM so this works with "Visual Studio Build Tools 2022" as well as the full IDE.
REM
REM NOTE: deliberately no setlocal - the environment variables that vcvars64.bat
REM sets must survive into the caller.
REM ---------------------------------------------------------------------------

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" goto :no_vswhere

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"

if not defined VSPATH goto :no_toolset

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto :vcvars_failed

echo [msvc_env] MSVC environment ready - %VSPATH%
exit /b 0

:no_vswhere
echo [msvc_env] vswhere.exe was not found, so no Visual Studio installation exists yet.
echo [msvc_env] Install "Visual Studio Build Tools 2022" and select the
echo [msvc_env] "Desktop development with C++" workload, or run this from an
echo [msvc_env] "x64 Native Tools Command Prompt for VS".
exit /b 1

:no_toolset
echo [msvc_env] Found Visual Studio but not the MSVC C++ toolset (component
echo [msvc_env] Microsoft.VisualStudio.Component.VC.Tools.x86.x64).
echo [msvc_env] Add the "Desktop development with C++" workload.
exit /b 1

:vcvars_failed
echo [msvc_env] Failed to initialise the MSVC environment from "%VSPATH%".
exit /b 1

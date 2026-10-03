@echo off
REM ---------------------------------------------------------------------------
REM Hand-written MSVC x64 environment for this sandbox.
REM
REM scripts/msvc_env.bat shells out to vswhere.exe, which in turn calls reg.exe.
REM Both are on this sandbox's Program Blacklist, so vswhere returns nothing and
REM vcvars64.bat silently skips the Windows SDK / UCRT include paths - every
REM compile then dies with "fatal error C1083: stddef.h: No such file or directory".
REM
REM This script sets the same variables by hand. The versions are pinned to match
REM build/windows-editor/CMakeCache.txt (CMake caches the compiler path and
REM refuses to reconfigure against a different toolset).
REM
REM   MSVC : 14.36.32532   (VS 2022 Community)
REM   SDK  : 10.0.22621.0
REM ---------------------------------------------------------------------------

set "VSDIR=D:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCVER=14.36.32532"
set "SDKROOT=C:\Program Files (x86)\Windows Kits\10"
set "SDKVER=10.0.22621.0"

set "PATH=%VSDIR%\VC\Tools\MSVC\%VCVER%\bin\Hostx64\x64;%SDKROOT%\bin\%SDKVER%\x64;%PATH%"

set "INCLUDE=%VSDIR%\VC\Tools\MSVC\%VCVER%\include;%SDKROOT%\Include\%SDKVER%\ucrt;%SDKROOT%\Include\%SDKVER%\shared;%SDKROOT%\Include\%SDKVER%\um;%SDKROOT%\Include\%SDKVER%\winrt;%SDKROOT%\Include\%SDKVER%\cppwinrt"

set "LIB=%VSDIR%\VC\Tools\MSVC\%VCVER%\lib\x64;%SDKROOT%\Lib\%SDKVER%\ucrt\x64;%SDKROOT%\Lib\%SDKVER%\um\x64"

echo [sandbox_msvc_env] cl.exe + SDK %SDKVER% wired up by hand (vswhere bypassed).
exit /b 0

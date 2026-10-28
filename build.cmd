@echo off
rem ===========================================================================
rem  AgentKeepAwake build script (MinGW-w64 / TDM-GCC)
rem
rem  Usage:
rem    build.cmd                           auto-detect compiler
rem    set GCC_BIN=D:\minGW\bin & build.cmd
rem
rem  IMPORTANT: -fno-exceptions -fno-rtti -fno-threadsafe-statics are required.
rem  Without them Windows Defender flags the exe as Trojan:Win64/SpyLoader!pz.
rem  Close the running AgentKeepAwake.exe before building (file lock).
rem ===========================================================================
setlocal enabledelayedexpansion
set "BIN=%GCC_BIN%"
set "GXX="
set "WINDRES="

where g++ >nul 2>nul
if not errorlevel 1 (
  set "GXX=g++"
  set "WINDRES=windres"
  goto :build
)

if not defined BIN call :find E:\
if not defined BIN call :find D:\
if not defined BIN call :find C:\
if not defined BIN goto :nocc

set "GXX=!BIN!\g++.exe"
set "WINDRES=!BIN!\windres.exe"

:build
cd /d "%~dp0src"
if errorlevel 1 goto :nosrc

"%WINDRES%" res.rc -o res.o
if errorlevel 1 goto :err

"%GXX%" -municode -mwindows -O2 -s -fno-exceptions -fno-rtti -fno-threadsafe-statics AgentKeepAwake4.cpp res.o -o AgentKeepAwake.exe -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32
if errorlevel 1 goto :err

del res.o >nul 2>nul
copy /y AgentKeepAwake.exe "%~dp0AgentKeepAwake.exe" >nul
echo.
echo [OK] %~dp0AgentKeepAwake.exe
exit /b 0

:err
del res.o >nul 2>nul
echo.
echo [FAIL] Build failed.
echo   - Is AgentKeepAwake.exe still running? Close it first.
echo   - Compiler missing? set GCC_BIN=path\to\mingw\bin and retry.
exit /b 1

:nosrc
echo [FAIL] src folder not found next to build.cmd
exit /b 1

:nocc
echo [FAIL] g++ not found in PATH or common locations.
echo        set GCC_BIN=path\to\mingw\bin and retry.
exit /b 1

rem ---- search 2 levels for a TDM-GCC / mingw64 toolchain (ASCII wildcards,
rem      works with non-ASCII parent folder names) --------------------------
:find
for /d %%a in ("%~1*") do (
  if /i "%%~nxa"=="TDM-GCC" if exist "%%~a\bin\g++.exe" set "BIN=%%~a\bin"
  if /i "%%~nxa"=="mingw64" if exist "%%~a\bin\g++.exe" set "BIN=%%~a\bin"
  for /d %%b in ("%%~a\*") do (
    if /i "%%~nxb"=="TDM-GCC" if exist "%%~b\bin\g++.exe" set "BIN=%%~b\bin"
    if /i "%%~nxb"=="mingw64" if exist "%%~b\bin\g++.exe" set "BIN=%%~b\bin"
  )
)
exit /b 0

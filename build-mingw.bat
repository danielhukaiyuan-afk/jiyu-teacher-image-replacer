@echo off
rem ============================================================================
rem ScreenReplace build script -- MinGW (Dev-C++ bundled GCC)
rem Output: out\ScreenReplace.exe + out\ScreenReplaceHooks.dll
rem NOTE: ASCII-only on purpose (cmd.exe reads .bat with the OEM codepage).
rem ============================================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem --- locate MinGW: 1) MINGW env var  2) Dev-C++ registry  3) common paths  4) PATH
if not defined MINGW (
  for /f "usebackq tokens=2,*" %%a in (`reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Dev-C++" /v UninstallString 2^>nul`) do set "UNINST=%%b"
  if defined UNINST for %%i in ("!UNINST!") do set "DEVDIR=%%~dpi"
  if defined DEVDIR if exist "!DEVDIR!MinGW32\bin\g++.exe" set "MINGW=!DEVDIR!MinGW32"
  if defined DEVDIR if exist "!DEVDIR!MinGW64\bin\g++.exe" set "MINGW=!DEVDIR!MinGW64"
)
if not defined MINGW (
  for %%d in ("%ProgramFiles%\Dev-Cpp" "%ProgramFiles(x86)%\Dev-Cpp" "C:\Dev-Cpp" "D:\Dev-Cpp" "%LOCALAPPDATA%\Programs\Dev-Cpp") do (
    if exist "%%~d\MinGW32\bin\g++.exe" set "MINGW=%%~d\MinGW32"
    if exist "%%~d\MinGW64\bin\g++.exe" set "MINGW=%%~d\MinGW64"
  )
)
if not defined MINGW (
  for /f "delims=" %%p in ('where g++ 2^>nul') do (
    for %%q in ("%%~dp%%p") do set "MINGW=%%~dq"
    for %%q in ("!MINGW!") do set "MINGW=%%~dqq"
  )
)
if not defined MINGW (
  echo [X] MinGW not found. Set it manually, for example:
  echo     set MINGW=D:\your\Dev-Cpp\MinGW32
  echo     build-mingw.bat
  exit /b 1
)
if not exist "%MINGW%\bin\g++.exe" (
  echo [X] g++.exe not found under "%MINGW%\bin"
  exit /b 1
)

rem CRITICAL: MinGW bin must be on PATH, otherwise cc1plus.exe fails silently (0xC0000135)
set "PATH=%MINGW%\bin;%PATH%"
echo [i] MinGW: %MINGW%

if not exist out mkdir out
if not exist out\obj mkdir out\obj

set "CXXFLAGS=-O2 -std=gnu++14 -DUNICODE -D_UNICODE -DWINVER=0x0601 -D_WIN32_WINNT=0x0601"
set "LDFLAGS=-static-libgcc -static-libstdc++"

echo [1/4] compiling mhook + disasm-lib ...
g++ -c %CXXFLAGS% -D"ODPRINTF(a)=" -include stddef.h src\mhook-lib\mhook.cpp -o out\obj\mhook.o || goto :err
gcc -c -O2 src\disasm-lib\cpu.c        -o out\obj\cpu.o        || goto :err
gcc -c -O2 src\disasm-lib\disasm.c     -o out\obj\disasm.o     || goto :err
gcc -c -O2 src\disasm-lib\disasm_x86.c -o out\obj\disasm_x86.o || goto :err
gcc -c -O2 src\disasm-lib\misc.c       -o out\obj\misc.o       || goto :err

echo [2/4] compiling SRHooks.cpp (core DLL) ...
pushd src
g++ -c %CXXFLAGS% -Iinclude SRHooks.cpp -o ..\out\obj\SRHooks.o
if errorlevel 1 ( popd & goto :err )
popd

echo [3/4] linking ScreenReplaceHooks.dll ...
g++ -shared -o out\ScreenReplaceHooks.dll ^
  out\obj\SRHooks.o out\obj\mhook.o out\obj\cpu.o ^
  out\obj\disasm.o out\obj\disasm_x86.o out\obj\misc.o ^
  %LDFLAGS% -lgdiplus -lshlwapi -lws2_32 -lgdi32 -luser32 -lkernel32 -ladvapi32 || goto :err

echo [4/4] compiling ScreenReplace.exe ...
pushd src\res
windres app.rc -O coff -o ..\..\out\obj\app.res
if errorlevel 1 ( popd & goto :err )
popd
g++ -o out\ScreenReplace.exe src\Main.cpp out\obj\app.res ^
  %CXXFLAGS% -Isrc\include -mwindows %LDFLAGS% ^
  -lcomctl32 -lcomdlg32 -ladvapi32 -lshell32 -lshlwapi -lole32 -lgdi32 -lgdiplus || goto :err

echo.
echo [OK] built:
echo   out\ScreenReplace.exe
echo   out\ScreenReplaceHooks.dll
exit /b 0

:err
echo.
echo [X] build failed - see compiler errors above.
exit /b 1

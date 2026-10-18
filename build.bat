@echo off
rem ============================================================================
rem ScreenReplace build script (MSVC / Visual Studio 2022, x86)
rem Output: out\ScreenReplace.exe + out\ScreenReplaceHooks.dll
rem ============================================================================
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
)
if not defined VSPATH (
  echo [X] 未找到 Visual Studio C++ 工具链。
  echo     请安装 VS2022 并勾选“使用 C++ 的桌面开发”组件后重试。
  exit /b 1
)

call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=x86 >nul
if errorlevel 1 exit /b 1

if not exist out mkdir out
if not exist out\obj mkdir out\obj

echo [1/4] 编译 mhook + disasm-lib ...
cl /nologo /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /utf-8 /c ^
   src\mhook-lib\mhook.cpp /Fo:out\obj\mhook.o || exit /b 1
cl /nologo /O2 /MT /W3 /utf-8 /c src\disasm-lib\cpu.c        /Fo:out\obj\cpu.o        || exit /b 1
cl /nologo /O2 /MT /W3 /utf-8 /c src\disasm-lib\disasm.c     /Fo:out\obj\disasm.o     || exit /b 1
cl /nologo /O2 /MT /W3 /utf-8 /c src\disasm-lib\disasm_x86.c /Fo:out\obj\disasm_x86.o || exit /b 1
cl /nologo /O2 /MT /W3 /utf-8 /c src\disasm-lib\misc.c       /Fo:out\obj\misc.o       || exit /b 1

echo [2/4] 编译 SRHooks.cpp ...
pushd src
cl /nologo /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /utf-8 /Iinclude /c ^
   SRHooks.cpp /Fo:..\out\obj\SRHooks.o
if errorlevel 1 ( popd & exit /b 1 )
popd

echo [3/4] 链接 ScreenReplaceHooks.dll ...
link /nologo /DLL /OUT:out\ScreenReplaceHooks.dll ^
  out\obj\SRHooks.o out\obj\mhook.o out\obj\cpu.o out\obj\disasm.o ^
  out\obj\disasm_x86.o out\obj\misc.o ^
  gdiplus.lib shlwapi.lib ws2_32.lib gdi32.lib user32.lib kernel32.lib advapi32.lib || exit /b 1

echo [4/4] 编译 ScreenReplace.exe ...
pushd src\res
rc.exe /nologo /fo ..\..\out\obj\app.res app.rc
if errorlevel 1 ( popd & exit /b 1 )
popd
cl /nologo /O2 /MT /EHsc /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /utf-8 /Isrc\include ^
   src\Main.cpp out\obj\app.res ^
   /Fe:out\ScreenReplace.exe ^
   /link comctl32.lib comdlg32.lib advapi32.lib shell32.lib shlwapi.lib gdiplus.lib gdi32.lib || exit /b 1

echo.
echo [OK] 编译完成：
echo   out\ScreenReplace.exe
echo   out\ScreenReplaceHooks.dll
exit /b 0

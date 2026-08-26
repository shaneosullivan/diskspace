@echo off
setlocal enabledelayedexpansion

rem Builds diskscan.exe and DiskSpace.exe with MSVC.
rem
rem   build.cmd            release
rem   build.cmd debug      debug, with the address sanitiser available
rem   build.cmd cli        only the command-line tool, which is the fast loop
rem
rem No project file and no CMake on purpose: the whole thing is two dozen
rem translation units, and a batch file that anyone can read beats a build system
rem nobody wants to debug.

set ROOT=%~dp0
set MODE=%1
if "%MODE%"=="" set MODE=release

rem Not written as an if-block: the path to vswhere contains parentheses, which
rem cmd mis-parses as the end of one.
if defined VSINSTALLDIR goto :haveToolchain
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :noToolchain
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto :noToolchain
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
goto :haveToolchain

:noToolchain
echo build: no Visual Studio C++ toolset found. Install the "Desktop development
echo        with C++" workload, or run this from a Developer Command Prompt.
exit /b 1

:haveToolchain

set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\cli" mkdir "%OUT%\cli"
if not exist "%OUT%\app" mkdir "%OUT%\app"

set COMMON=/nologo /std:c++20 /EHsc /W4 /wd4100 /wd4127 /wd4201 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX
if /i "%MODE%"=="debug" (
  set FLAGS=%COMMON% /Od /Zi /MDd /RTC1 /D_DEBUG
  set LINKFLAGS=/DEBUG
) else (
  rem /GL plus /LTCG lets the compiler inline the arena accessors into the
  rem scanner's inner loops, which is where the whole cost of a scan lives.
  set FLAGS=%COMMON% /O2 /Oi /GL /MD /DNDEBUG /GS-
  set LINKFLAGS=/LTCG /OPT:REF /OPT:ICF
)

set CORE=^
 "%ROOT%src\core\node_arena.cpp" ^
 "%ROOT%src\core\file_kind.cpp" ^
 "%ROOT%src\core\queues.cpp" ^
 "%ROOT%src\core\tree_scanner.cpp" ^
 "%ROOT%src\core\mft_scanner.cpp" ^
 "%ROOT%src\core\scan_history.cpp" ^
 "%ROOT%src\core\win_util.cpp"

set APP=^
 "%ROOT%src\app\main.cpp" ^
 "%ROOT%src\app\treemap.cpp" ^
 "%ROOT%src\app\treemap_view.cpp" ^
 "%ROOT%src\app\tree_pane.cpp" ^
 "%ROOT%src\app\kind_pane.cpp" ^
 "%ROOT%src\app\welcome_view.cpp" ^
 "%ROOT%src\app\preferences.cpp" ^
 "%ROOT%src\app\preferences_window.cpp" ^
 "%ROOT%src\app\update_check.cpp" ^
 "%ROOT%src\app\panels.cpp" ^
 "%ROOT%src\app\ui_common.cpp" ^
 "%ROOT%src\app\scan_session.cpp"

echo building diskscan.exe (%MODE%)
cl %FLAGS% /Fo"%OUT%\cli\\" /Fe"%OUT%\diskscan.exe" %CORE% "%ROOT%src\cli\diskscan.cpp" /link %LINKFLAGS% /SUBSYSTEM:CONSOLE || exit /b 1

if /i "%1"=="cli" goto :done

echo building DiskSpace.exe (%MODE%)
rc /nologo /fo "%OUT%\app.res" "%ROOT%src\app\app.rc" || exit /b 1
rem The GUI libraries are named here rather than with #pragma comment, because the
rem same core objects are also linked into the console tool, which needs none of
rem them.
set GUILIBS=user32.lib gdi32.lib shell32.lib ole32.lib comdlg32.lib comctl32.lib uxtheme.lib dwmapi.lib advapi32.lib
cl %FLAGS% /Fo"%OUT%\app\\" /Fe"%OUT%\DiskSpace.exe" %CORE% %APP% "%OUT%\app.res" /link %LINKFLAGS% %GUILIBS% /SUBSYSTEM:WINDOWS || exit /b 1

:done
echo.
echo built into %OUT%

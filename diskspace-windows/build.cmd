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

rem Finding the toolchain.
rem
rem The usual recipe wraps vswhere in a `for /f` with backticks, which needs the
rem path to it quoted - and that path contains both spaces and parentheses, which
rem is exactly the shape cmd's quote handling is worst at. The known install
rem locations are probed directly instead: no subprocess, no quoting, and a
rem definite answer. vswhere remains as a fallback for an install somewhere
rem unusual, reading through a temp file rather than a pipe.
if defined VSINSTALLDIR goto :haveToolchain

rem Copied into names of their own first. "%ProgramFiles(x86)%" contains a closing
rem parenthesis, and inside a parenthesised block cmd treats that as the end of
rem the block - so the probe below has to never mention it directly.
set "PF64=%ProgramFiles%"
set "PF32=%ProgramFiles(x86)%"

rem Flat `if ... if ... set` rather than if/else. An `) else if ... (` chain
rem nested two blocks deep parses wrongly here and silently matches nothing,
rem which sends this straight to the fallback below without saying why.
set "VCVARS="
for %%r in (2022 2019) do (
  for %%e in (Enterprise Professional Community BuildTools Preview) do (
    if not defined VCVARS if exist "!PF64!\Microsoft Visual Studio\%%r\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PF64!\Microsoft Visual Studio\%%r\%%e\VC\Auxiliary\Build\vcvars64.bat"
    if not defined VCVARS if exist "!PF32!\Microsoft Visual Studio\%%r\%%e\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=!PF32!\Microsoft Visual Studio\%%r\%%e\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if defined VCVARS goto :runVcvars

set "VSWHERE=%PF32%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :noToolchain
set "VSWHERE_OUT=%TEMP%\diskspace-vswhere-%RANDOM%.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSWHERE_OUT%" 2>nul
for /f "usebackq tokens=*" %%i in ("%VSWHERE_OUT%") do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
del "%VSWHERE_OUT%" >nul 2>&1
if not defined VCVARS goto :noToolchain
if not exist "%VCVARS%" goto :noToolchain

:runVcvars
rem Both streams: vcvars64.bat writes chatter of its own to stderr on some
rem installs - including a harmless complaint about vswhere - and a caller that
rem treats any stderr as failure would take that for a broken build.
call "%VCVARS%" >nul 2>&1
if errorlevel 1 goto :noToolchain
goto :haveToolchain

:noToolchain
echo build: no Visual Studio C++ toolset found. Install the "Desktop development
echo        with C++" workload, or run this from a Developer Command Prompt.
exit /b 1

:haveToolchain

rem The platform-neutral half of the scanner lives above this directory, shared
rem with the Linux build. Reached by an include path rather than by rewritten
rem #include lines, so moving a file there needed no edit to find its neighbours.
set SHARED=%ROOT%..\core

set OUT=%ROOT%build
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\cli" mkdir "%OUT%\cli"
if not exist "%OUT%\app" mkdir "%OUT%\app"

set COMMON=/nologo /std:c++20 /EHsc /W4 /wd4100 /wd4127 /wd4201 /utf-8 /I "%SHARED%" /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX
if /i "%MODE%"=="debug" (
  set FLAGS=%COMMON% /Od /Zi /MTd /RTC1 /D_DEBUG
  set LINKFLAGS=/DEBUG
) else (
  rem /GL plus /LTCG lets the compiler inline the arena accessors into the
  rem scanner's inner loops, which is where the whole cost of a scan lives.
  rem /MT rather than /MD: the C runtime is linked in rather than imported, so
  rem the executable needs no Visual C++ Redistributable installed. A download
  rem billed as a portable single file has to run on a machine that has never had
  rem a developer tool near it, and the few hundred kilobytes this costs are
  rem worth more than that. The price is that a CRT security fix needs a rebuild
  rem here rather than arriving through Windows Update.
  set FLAGS=%COMMON% /O2 /Oi /GL /MT /DNDEBUG /GS-
  set LINKFLAGS=/LTCG /OPT:REF /OPT:ICF
)

set CORE=^
 "%SHARED%\node_arena.cpp" ^
 "%SHARED%\file_kind.cpp" ^
 "%SHARED%\queues.cpp" ^
 "%SHARED%\tree_scanner.cpp" ^
 "%ROOT%src\core\tree_scanner_win.cpp" ^
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
cl %FLAGS% /Fo"%OUT%\cli\\" /Fe"%OUT%\diskscan.exe" %CORE% "%ROOT%..\cli\diskscan.cpp" /link %LINKFLAGS% /SUBSYSTEM:CONSOLE || exit /b 1

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

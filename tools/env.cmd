@echo off
rem Sets up the build environment for the calling script:  call "%~dp0tools\env.cmd" || exit /b 1
rem  - Visual Studio's x64 tools (clang-cl, lld, Ninja, CMake): the newest Visual Studio with the C++
rem    Clang tools, found through vswhere, or VCVARS if it's set to a vcvars64.bat. Calling vcvars64.bat
rem    can change the current directory.
rem  - XENONRECOMP_ROOT, the XenonRecomp checkout: a sibling of this repo unless already set, or in a linked
rem    worktree (Claude Code makes them in .claude\worktrees) a sibling of the main checkout.
if defined XENONRECOMP_ROOT goto xenonrecomp
for %%i in ("%~dp0..\..\XenonRecomp") do set "XENONRECOMP_ROOT=%%~fi"
if exist "%XENONRECOMP_ROOT%\thirdparty\simde" goto xenonrecomp
for /f "usebackq delims=" %%i in (`git -C "%~dp0.." rev-parse --path-format^=absolute --git-common-dir 2^>nul`) do for %%j in ("%%~i\..\..\XenonRecomp") do set "XENONRECOMP_ROOT=%%~fj"
:xenonrecomp
if defined VCVARS goto found
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto missing
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
:found
if not exist "%VCVARS%" goto missing
call "%VCVARS%" >nul 2>&1
exit /b 0
:missing
echo No Visual Studio with the C++ Clang tools found; install them or set VCVARS to a vcvars64.bat 1>&2
exit /b 1

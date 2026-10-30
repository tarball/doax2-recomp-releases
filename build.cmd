@echo off
rem Configures (if needed) and builds the runtime with clang-cl, Ninja and lld.
rem Usage: build.cmd [Release|RelWithDebInfo|Debug]   (default RelWithDebInfo)
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo
call "%~dp0tools\env.cmd" || exit /b 1
cd /d "%~dp0"
set BUILD_DIR=build\%CONFIG%
if not exist "%BUILD_DIR%\build.ninja" (
    cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% ^
        -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER_TYPE=LLD ^
        "-DXENONRECOMP_ROOT:PATH=%XENONRECOMP_ROOT%" || exit /b 1
)
cmake --build "%BUILD_DIR%" || exit /b 1

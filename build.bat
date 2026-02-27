@echo off
echo ========================================
echo Building Skeleton Vulkan (Windows)
echo ========================================
echo.

set CMAKE_PATH="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set MSBUILD_PATH="C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"
set BUILD_DIR=%~dp0build
set SOURCE_DIR=%~dp0

echo Build directory: %BUILD_DIR%
echo.

REM Configure
echo [1/2] Configuring CMake...
echo.

%CMAKE_PATH% -B %BUILD_DIR% -S %SOURCE_DIR% ^
    -DCMAKE_BUILD_TYPE=Release

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] CMake configuration failed!
    pause
    exit /b 1
)

echo.
echo [2/2] Building...
echo.

REM Build via MSBuild for proper /MT static linking
%MSBUILD_PATH% "%BUILD_DIR%\SkeletonVulkan.vcxproj" ^
    -p:Configuration=Release ^
    -p:Platform=x64 ^
    -p:RuntimeLibrary=MultiThreaded

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESSFUL
echo ========================================
echo.
echo Output: build\Release\SkeletonVulkan.aex
echo.
echo Install by copying to:
echo   C:\Program Files\Adobe\Adobe After Effects ^<version^>\Support Files\Plug-ins\
echo.
pause

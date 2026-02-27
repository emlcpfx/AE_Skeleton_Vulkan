@echo off
REM Compile GLSL compute shader to SPIR-V and generate C++ header
REM
REM Prerequisites:
REM   - Vulkan SDK installed (provides glslc)
REM   - VULKAN_SDK environment variable set, or glslc on PATH
REM
REM Usage:
REM   compile_shader.bat

setlocal

REM Find glslc
where glslc >nul 2>&1
if %ERRORLEVEL% neq 0 (
    if defined VULKAN_SDK (
        set "GLSLC=%VULKAN_SDK%\Bin\glslc.exe"
    ) else (
        echo ERROR: glslc not found. Install the Vulkan SDK and set VULKAN_SDK.
        exit /b 1
    )
) else (
    set "GLSLC=glslc"
)

echo Compiling gain.comp to SPIR-V...
%GLSLC% gain.comp -o gain.spv
if %ERRORLEVEL% neq 0 (
    echo ERROR: Shader compilation failed.
    exit /b 1
)

echo Generating C++ header...
REM Generate a C++ byte array header from the SPIR-V binary
REM Since xxd is not available on Windows, we use a PowerShell one-liner

powershell -Command ^
    "$bytes = [System.IO.File]::ReadAllBytes('gain.spv'); " ^
    "$sb = [System.Text.StringBuilder]::new(); " ^
    "[void]$sb.AppendLine('#pragma once'); " ^
    "[void]$sb.AppendLine('// Auto-generated from gain.comp - do not edit manually'); " ^
    "[void]$sb.AppendLine('// Recompile with: compile_shader.bat'); " ^
    "[void]$sb.AppendLine(''); " ^
    "[void]$sb.AppendLine('static const unsigned char gain_shader_spv[] = {'); " ^
    "$line = '    '; " ^
    "for ($i = 0; $i -lt $bytes.Length; $i++) { " ^
    "  $line += '0x' + $bytes[$i].ToString('x2'); " ^
    "  if ($i -lt $bytes.Length - 1) { $line += ', ' }; " ^
    "  if (($i + 1) %% 16 -eq 0) { [void]$sb.AppendLine($line); $line = '    ' } " ^
    "}; " ^
    "if ($line.Trim().Length -gt 0) { [void]$sb.AppendLine($line) }; " ^
    "[void]$sb.AppendLine('};'); " ^
    "[void]$sb.AppendLine(''); " ^
    "[void]$sb.AppendLine('static const unsigned int gain_shader_spv_len = ' + $bytes.Length + ';'); " ^
    "[System.IO.File]::WriteAllText('..\src\gpu\gain_shader_spv.h', $sb.ToString())"

if %ERRORLEVEL% neq 0 (
    echo ERROR: Header generation failed.
    exit /b 1
)

echo Done! Output:
echo   gain.spv                    (SPIR-V binary)
echo   ..\src\gpu\gain_shader_spv.h (C++ header)

endlocal

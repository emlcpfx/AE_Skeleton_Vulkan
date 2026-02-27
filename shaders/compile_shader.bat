@echo off
REM Compile GLSL compute shader to SPIR-V for all pixel formats
REM and generate a combined C++ header with embedded byte arrays.
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

echo Compiling gain.comp to SPIR-V (3 format variants)...

REM 8-bit: rgba8 / VK_FORMAT_R8G8B8A8_UNORM
%GLSLC% -DIMAGE_FORMAT=rgba8 gain.comp -o gain_8.spv
if %ERRORLEVEL% neq 0 (
    echo ERROR: 8-bit shader compilation failed.
    exit /b 1
)
echo   gain_8.spv     (8-bit rgba8)

REM 16-bit: rgba16 / VK_FORMAT_R16G16B16A16_UNORM
%GLSLC% -DIMAGE_FORMAT=rgba16 gain.comp -o gain_16.spv
if %ERRORLEVEL% neq 0 (
    echo ERROR: 16-bit shader compilation failed.
    exit /b 1
)
echo   gain_16.spv    (16-bit rgba16)

REM 32-bit float: rgba32f / VK_FORMAT_R32G32B32A32_SFLOAT
%GLSLC% -DIMAGE_FORMAT=rgba32f gain.comp -o gain_float.spv
if %ERRORLEVEL% neq 0 (
    echo ERROR: 32-bit float shader compilation failed.
    exit /b 1
)
echo   gain_float.spv (32-bit rgba32f)

echo.
echo Generating combined C++ header...

REM Generate a C++ header with all three SPIR-V byte arrays
powershell -Command ^
    "function ConvertToArray($file, $name) {" ^
    "  $b = [IO.File]::ReadAllBytes($file);" ^
    "  $s = [Text.StringBuilder]::new();" ^
    "  [void]$s.AppendLine(\"static const unsigned char ${name}[] = {\");" ^
    "  $l = '    ';" ^
    "  for ($i=0; $i -lt $b.Length; $i++) {" ^
    "    $l += '0x'+$b[$i].ToString('x2');" ^
    "    if ($i -lt $b.Length-1) {$l+=', '};" ^
    "    if (($i+1)%%16 -eq 0) {[void]$s.AppendLine($l);$l='    '}" ^
    "  };" ^
    "  if ($l.Trim().Length -gt 0) {[void]$s.AppendLine($l)};" ^
    "  [void]$s.AppendLine('};');" ^
    "  [void]$s.AppendLine('');" ^
    "  [void]$s.AppendLine(\"static const unsigned int ${name}_len = \"+$b.Length+';');" ^
    "  return $s.ToString()" ^
    "}" ^
    "$h = [Text.StringBuilder]::new();" ^
    "[void]$h.AppendLine('#pragma once');" ^
    "[void]$h.AppendLine('// Auto-generated from gain.comp - do not edit manually');" ^
    "[void]$h.AppendLine('// Recompile with: cd shaders && compile_shader.bat');" ^
    "[void]$h.AppendLine('');" ^
    "[void]$h.AppendLine('// 8-bit (rgba8 / VK_FORMAT_R8G8B8A8_UNORM)');" ^
    "[void]$h.Append((ConvertToArray 'gain_8.spv' 'gain_shader_8_spv'));" ^
    "[void]$h.AppendLine('');" ^
    "[void]$h.AppendLine('// 16-bit (rgba16 / VK_FORMAT_R16G16B16A16_UNORM)');" ^
    "[void]$h.Append((ConvertToArray 'gain_16.spv' 'gain_shader_16_spv'));" ^
    "[void]$h.AppendLine('');" ^
    "[void]$h.AppendLine('// 32-bit float (rgba32f / VK_FORMAT_R32G32B32A32_SFLOAT)');" ^
    "[void]$h.Append((ConvertToArray 'gain_float.spv' 'gain_shader_float_spv'));" ^
    "[IO.File]::WriteAllText('..\src\gpu\gain_shader_spv.h', $h.ToString())"

if %ERRORLEVEL% neq 0 (
    echo ERROR: Header generation failed.
    exit /b 1
)

echo.
echo Done! Output:
echo   gain_8.spv                     (8-bit SPIR-V)
echo   gain_16.spv                    (16-bit SPIR-V)
echo   gain_float.spv                 (32-bit float SPIR-V)
echo   ..\src\gpu\gain_shader_spv.h   (combined C++ header)

endlocal

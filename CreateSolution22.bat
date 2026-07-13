@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOTDIR=%~dp0"
set "BUILDDIR=build_intermediate"
set "BINDIR=game"

pushd "%ROOTDIR%" || exit /b 1
set "PUSHD_DONE=1"

call :read_build_version
if errorlevel 1 goto :fail

call :find_cmake
if errorlevel 1 goto :fail
echo Using CMake: %CMAKE%

call :select_generator
if errorlevel 1 goto :fail

if not exist "%BUILDDIR%" (
  mkdir "%BUILDDIR%"
)
if not exist "%BINDIR%" (
  mkdir "%BINDIR%"
)

if exist "%BUILDDIR%\CMakeCache.txt" (
  set "CLEAR_CMAKE_CACHE=0"
  findstr /C:"CMAKE_GENERATOR:INTERNAL=%CMAKE_GENERATOR%" "%BUILDDIR%\CMakeCache.txt" >nul 2>nul
  if errorlevel 1 (
    set "CLEAR_CMAKE_CACHE=1"
  )
  findstr /C:"CMAKE_GENERATOR_PLATFORM:INTERNAL=x64" "%BUILDDIR%\CMakeCache.txt" >nul 2>nul
  if errorlevel 1 (
    set "CLEAR_CMAKE_CACHE=1"
  )
  for /f "tokens=2 delims==" %%I in ('findstr /B /C:"CMAKE_GENERATOR_INSTANCE:INTERNAL=" "%BUILDDIR%\CMakeCache.txt" 2^>nul') do (
    if not "%%~I"=="" (
      if not exist "%%~I" (
        set "CLEAR_CMAKE_CACHE=1"
      )
    )
  )
  if "!CLEAR_CMAKE_CACHE!"=="1" (
    echo Existing CMake cache uses another generator, platform, or Visual Studio instance; removing stale cache.
    rmdir /s /q "%BUILDDIR%\CMakeFiles" >nul 2>nul
    del /q "%BUILDDIR%\CMakeCache.txt" >nul 2>nul
  )
)

"%CMAKE%" -S . -B "%BUILDDIR%" -G "%CMAKE_GENERATOR%" -A x64
if errorlevel 1 goto :fail

call :write_solution_manifest
if errorlevel 1 goto :fail

echo Finished generating solution files for source version %SDK_BUILD_VERSION%.
goto :success

:success
set "EXITCODE=0"
goto :finish

:fail
set "EXITCODE=1"
goto :finish

:finish
if defined PUSHD_DONE popd
endlocal & exit /b %EXITCODE%

:read_build_version
set "SDK_BUILD_VERSION="
if not exist "src\core\build_version.h" (
  echo Could not find src\core\build_version.h.
  exit /b 1
)
for /f "tokens=2 delims==" %%V in ('findstr /C:"SDK_INTERNAL_BUILD_NUMBER" "src\core\build_version.h"') do (
  set "SDK_BUILD_VERSION=%%V"
)
set "SDK_BUILD_VERSION=!SDK_BUILD_VERSION: =!"
set "SDK_BUILD_VERSION=!SDK_BUILD_VERSION:u=!"
set "SDK_BUILD_VERSION=!SDK_BUILD_VERSION:;=!"
if not defined SDK_BUILD_VERSION (
  echo Could not read SDK_INTERNAL_BUILD_NUMBER from src\core\build_version.h.
  exit /b 1
)
exit /b 0

:write_solution_manifest
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$buildDir = Join-Path (Get-Location) $env:BUILDDIR; $manifest = [ordered]@{ version = [int64]$env:SDK_BUILD_VERSION; source_version = [int64]$env:SDK_BUILD_VERSION; generator = $env:CMAKE_GENERATOR; platform = 'x64'; build_dir = (Resolve-Path -LiteralPath $buildDir).Path; generated_at = (Get-Date).ToString('o') }; $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $buildDir '.solution_manifest.json') -Encoding UTF8"
if errorlevel 1 (
  echo Failed to write solution manifest.
  exit /b 1
)
exit /b 0

:find_cmake
set "CMAKE="
for %%P in (
    "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
) do (
    if exist "%%~P" (
        set "CMAKE=%%~P"
        exit /b 0
    )
)

where cmake >nul 2>nul
if "%ERRORLEVEL%"=="0" (
    set "CMAKE=cmake"
    exit /b 0
)

echo Could not find cmake.exe. Add CMake to PATH or install CMake tools in Visual Studio Installer.
exit /b 1

:select_generator
REM Prefer newer Visual Studio generators. VS 18 is the VS 2026 generator;
REM selecting VS 17 on a VS 18 machine makes CMake request v143.
for %%V in (18 17 16 15) do (
    call :select_generator_from_vswhere_major %%V
    if not errorlevel 1 exit /b 0
    call :has_vs_install_dir %%V
    if NOT ERRORLEVEL 1 (
        call :set_generator %%V
        if not errorlevel 1 exit /b 0
    )
    reg query "HKEY_CLASSES_ROOT\VisualStudio.DTE.%%V.0" >nul 2>nul
    if NOT ERRORLEVEL 1 (
        call :set_generator %%V
        if not errorlevel 1 exit /b 0
    )
    call :cmake_has_generator %%V
    if not errorlevel 1 exit /b 0
)

echo Could not find a supported version of Visual Studio; exiting...
exit /b 1

:select_generator_from_vswhere_major
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1

for /f "usebackq tokens=1 delims=." %%V in (`"%VSWHERE%" -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion 2^>nul`) do (
    if "%%V"=="%~1" (
        call :set_generator %%V
        if not errorlevel 1 exit /b 0
    )
)
exit /b 1

:cmake_has_generator
call :generator_name %~1
if errorlevel 1 exit /b 1

set "GENERATOR_TO_CHECK=%CMAKE_GENERATOR%"
set "CMAKE_GENERATOR="
"%CMAKE%" --help 2>nul | findstr /C:"%GENERATOR_TO_CHECK%" >nul 2>nul
if errorlevel 1 (
    set "GENERATOR_TO_CHECK="
    exit /b 1
)
set "CMAKE_GENERATOR=%GENERATOR_TO_CHECK%"
set "GENERATOR_TO_CHECK="

echo Using Visual Studio %~1 as generator.
exit /b 0

:has_vs_install_dir
if "%~1"=="18" (
    if exist "%ProgramFiles%\Microsoft Visual Studio\18" exit /b 0
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18" exit /b 0
)
if "%~1"=="17" (
    if exist "%ProgramFiles%\Microsoft Visual Studio\2022" exit /b 0
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022" exit /b 0
)
if "%~1"=="16" (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019" exit /b 0
)
if "%~1"=="15" (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017" exit /b 0
)
exit /b 1

:set_generator
call :generator_name %~1
if errorlevel 1 exit /b 1

echo Using Visual Studio %~1 as generator.
exit /b 0

:generator_name
set "CMAKE_GENERATOR="
if "%~1"=="15" (
    set "CMAKE_GENERATOR=Visual Studio 15 2017"
)
if "%~1"=="16" (
    set "CMAKE_GENERATOR=Visual Studio 16 2019"
)
if "%~1"=="17" (
    set "CMAKE_GENERATOR=Visual Studio 17 2022"
)
if "%~1"=="18" (
    set "CMAKE_GENERATOR=Visual Studio 18 2026"
)

if defined CMAKE_GENERATOR (
    exit /b 0
)

exit /b 1

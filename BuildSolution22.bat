@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SELF=%~f0"
set "ORIGINAL_ARGS=%*"

set "REQUESTED_CONFIG=%~1"
if "%REQUESTED_CONFIG%"=="" (
    echo Usage: %~nx0 Debug^|Release [target] [--no-generate]
    exit /b 1
)

if /I "%REQUESTED_CONFIG%"=="Debug" (
    set "CONFIG=Debug"
    set "LOG_PREFIX=debug_build"
    set "LATEST_LOG=debug_latest.log"
    set "SYMBOLS_ROOT_NAME=symbols_debug"
) else if /I "%REQUESTED_CONFIG%"=="Release" (
    set "CONFIG=Release"
    set "LOG_PREFIX=build"
    set "LATEST_LOG=latest.log"
    set "SYMBOLS_ROOT_NAME=symbols_release"
) else (
    echo Unsupported build configuration: %REQUESTED_CONFIG%
    exit /b 1
)

set "TARGET=ALL_BUILD"
set "TARGET_SET=0"
set "GENERATE_SOLUTION=1"
shift

:parse_args
if "%~1"=="" goto args_done
set "ARG=%~1"
if /I "!ARG!"=="--no-generate" (
    set "GENERATE_SOLUTION=0"
    shift
    goto parse_args
)
if /I "!ARG!"=="--skip-generate" (
    set "GENERATE_SOLUTION=0"
    shift
    goto parse_args
)
if /I "!ARG!"=="--generate" (
    set "GENERATE_SOLUTION=1"
    shift
    goto parse_args
)
if "!ARG:~0,2!"=="--" (
    echo Unsupported option: !ARG!
    exit /b 1
)
if "!TARGET_SET!"=="1" (
    echo Unexpected extra argument: !ARG!
    exit /b 1
)
set "TARGET=!ARG!"
set "TARGET_SET=1"
shift
goto parse_args

:args_done

if not defined R5SDK_BUILD_LOGGING (
    set "ROOTDIR=%~dp0"
    if defined R5SDK_BUILD_LOG_DIR (
        set "LOGDIR=!R5SDK_BUILD_LOG_DIR!"
    ) else (
        set "LOGDIR=!ROOTDIR!log"
    )
    if not exist "!LOGDIR!" mkdir "!LOGDIR!"
    for /f %%T in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "LOGSTAMP=%%T"
    if not defined LOGSTAMP set "LOGSTAMP=!RANDOM!!RANDOM!"
    set "LOGFILE=!LOGDIR!\!LOG_PREFIX!_!LOGSTAMP!.log"
    set "EXITFILE=%TEMP%\r5sdk_!LOG_PREFIX!_!LOGSTAMP!.exit"
    set "RUNNER=%TEMP%\r5sdk_!LOG_PREFIX!_!LOGSTAMP!.cmd"
    echo Writing !CONFIG! build log to: !LOGFILE!
    set "R5SDK_BUILD_LOGFILE=!LOGFILE!"
    if exist "!EXITFILE!" del /q "!EXITFILE!" >nul 2>nul
    if exist "!RUNNER!" del /q "!RUNNER!" >nul 2>nul
    (
        echo @echo off
        echo setlocal EnableExtensions EnableDelayedExpansion
        echo set "R5SDK_BUILD_LOGGING=1"
        echo set "BUILD_LOG_CODEPAGE=65001"
        echo set "LOGSTAMP=!LOGSTAMP!"
        echo call "%SELF%" %ORIGINAL_ARGS%
        echo echo ^^!ERRORLEVEL^^!^>"!EXITFILE!"
    ) > "!RUNNER!"
    cmd.exe /d /c ""!RUNNER!"" 2>&1 | powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$input | Tee-Object -FilePath $env:R5SDK_BUILD_LOGFILE"
    if exist "!EXITFILE!" (
        set /p EXITCODE=<"!EXITFILE!"
    ) else (
        set "EXITCODE=1"
    )
    del /q "!RUNNER!" >nul 2>nul
    del /q "!EXITFILE!" >nul 2>nul
    copy /y "!LOGFILE!" "!LOGDIR!\!LATEST_LOG!" >nul 2>nul
    if "!EXITCODE!"=="0" (
        echo !CONFIG! build succeeded. Log: !LOGFILE!
    ) else (
        echo !CONFIG! build failed with exit code !EXITCODE!. Log: !LOGFILE!
    )
    exit /b !EXITCODE!
)

set "ORIGINAL_CODEPAGE="
for /f "tokens=2 delims=:" %%C in ('chcp') do set "ORIGINAL_CODEPAGE=%%C"
set "ORIGINAL_CODEPAGE=%ORIGINAL_CODEPAGE: =%"
if defined BUILD_LOG_CODEPAGE chcp %BUILD_LOG_CODEPAGE% >nul 2>nul

set "ROOTDIR=%~dp0"
set "BUILDDIR=%ROOTDIR%build_intermediate"
set "GAMEDIR=%ROOTDIR%game"
set "ARTIFACTS_ROOT=%ROOTDIR%artifacts"
set "SYMBOLS_ROOT=%ROOTDIR%%SYMBOLS_ROOT_NAME%"
if not defined LOGSTAMP (
    for /f %%T in ('powershell.exe -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set "LOGSTAMP=%%T"
)
if not defined LOGSTAMP set "LOGSTAMP=!RANDOM!!RANDOM!"
set "SYMBOLSDIR=%SYMBOLS_ROOT%\%CONFIG%_%LOGSTAMP%"
set "VSCRIPT_SRC=%ROOTDIR%src\resource\vscripts"
set "VSCRIPT_DEST=%GAMEDIR%\platform\scripts\vscripts"
set "MOD_DEFAULTS_SRC=%ROOTDIR%src\resource\defaults\mods"
set "MOD_DEFAULTS_DEST=%GAMEDIR%\mods"
set "MOD_DEFAULTS_MANIFEST=%BUILDDIR%\.r5sdk_mod_defaults_manifest.txt"

pushd "%ROOTDIR%" || goto :fail
set "PUSHD_DONE=1"

call :read_build_version
if errorlevel 1 goto :fail

call :find_cmake
if errorlevel 1 goto :fail
echo Using CMake: %CMAKE%

call :archive_previous_outputs
if errorlevel 1 goto :fail

if "%GENERATE_SOLUTION%"=="1" (
    call "%ROOTDIR%CreateSolution22.bat"
    if errorlevel 1 goto :fail
) else (
    echo Skipping solution generation because --no-generate was specified.
)

echo Building %TARGET% [%CONFIG% x64]...
"%CMAKE%" --build "%BUILDDIR%" --config "%CONFIG%" --target "%TARGET%" -- /m
if errorlevel 1 goto :fail

call :sync_vscripts
if errorlevel 1 goto :fail

call :sync_mod_defaults
if errorlevel 1 goto :fail

call :separate_debug_files
if errorlevel 1 goto :fail

call :publish_artifacts
if errorlevel 1 goto :fail

echo Build finished. Deployable files are in "%GAMEDIR%".
echo Versioned artifacts are in "%ARTIFACT_VERSION_DIR%".
if exist "%SYMBOLSDIR%" echo Debug files are in "%SYMBOLSDIR%" and "%SYMBOLS_ROOT%\latest".
goto :success

:success
set "EXITCODE=0"
goto :finish

:fail
set "EXITCODE=1"
goto :finish

:finish
if defined PUSHD_DONE popd
if defined ORIGINAL_CODEPAGE chcp %ORIGINAL_CODEPAGE% >nul 2>nul
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

:read_json_version
set "%~2="
if not exist "%~1" exit /b 0
set "JSON_VERSION_FILE=%~1"
for /f "usebackq delims=" %%V in (`powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$p = $env:JSON_VERSION_FILE; try { $j = Get-Content -Raw -LiteralPath $p | ConvertFrom-Json; if ($null -ne $j.version) { $j.version } elseif ($null -ne $j.source_version) { $j.source_version } } catch { }"`) do (
  set "%~2=%%V"
)
set "JSON_VERSION_FILE="
exit /b 0

:archive_previous_outputs
set "ARCHIVE_ROOT=%BUILDDIR%\archive"
if not exist "%ARCHIVE_ROOT%" mkdir "%ARCHIVE_ROOT%"

set "OLD_GAME_VERSION=unknown"
call :read_json_version "%GAMEDIR%\.r5sdk_build.json" OLD_GAME_VERSION
if not defined OLD_GAME_VERSION set "OLD_GAME_VERSION=unknown"

if exist "%GAMEDIR%" (
    call :compress_directory "%GAMEDIR%" "%ARCHIVE_ROOT%\game_%OLD_GAME_VERSION%_%CONFIG%_%LOGSTAMP%.zip"
    if errorlevel 1 exit /b 1
)

if exist "%SYMBOLS_ROOT%\latest" (
    call :compress_directory "%SYMBOLS_ROOT%\latest" "%ARCHIVE_ROOT%\%SYMBOLS_ROOT_NAME%_%OLD_GAME_VERSION%_%CONFIG%_%LOGSTAMP%.zip"
    if errorlevel 1 exit /b 1
)

exit /b 0

:compress_directory
set "ARCHIVE_SOURCE=%~1"
set "ARCHIVE_DEST=%~2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$src = $env:ARCHIVE_SOURCE; $dst = $env:ARCHIVE_DEST; if (!(Test-Path -LiteralPath $src)) { exit 0 }; $items = Get-ChildItem -LiteralPath $src -Force -ErrorAction SilentlyContinue; if (!$items) { exit 0 }; New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null; Compress-Archive -Path (Join-Path $src '*') -DestinationPath $dst -Force; Write-Host ('Archived {0} -> {1}' -f $src, $dst)"
if errorlevel 1 (
    echo Failed to archive "%ARCHIVE_SOURCE%".
    exit /b 1
)
set "ARCHIVE_SOURCE="
set "ARCHIVE_DEST="
exit /b 0

:write_build_manifest
if not exist "%GAMEDIR%" mkdir "%GAMEDIR%"
set "BUILD_MANIFEST_GAME=%GAMEDIR%\.r5sdk_build.json"
set "BUILD_MANIFEST_ARTIFACT=%ARTIFACT_VERSION_DIR%\build_manifest.json"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$manifest = [ordered]@{ version = [int64]$env:SDK_BUILD_VERSION; source_version = [int64]$env:SDK_BUILD_VERSION; build_config = $env:CONFIG; target = $env:TARGET; status = 'success'; built_at = (Get-Date).ToString('o'); root_dir = $env:ROOTDIR; build_dir = $env:BUILDDIR; game_dir = $env:GAMEDIR; artifact_dir = $env:ARTIFACT_VERSION_DIR; game_zip = (Join-Path $env:ARTIFACT_VERSION_DIR 'game.zip'); symbols_debug_zip = (Join-Path $env:ARTIFACT_VERSION_DIR 'symbols_debug.zip') }; $json = $manifest | ConvertTo-Json -Depth 6; $json | Set-Content -LiteralPath $env:BUILD_MANIFEST_GAME -Encoding UTF8; $json | Set-Content -LiteralPath $env:BUILD_MANIFEST_ARTIFACT -Encoding UTF8"
if errorlevel 1 (
    echo Failed to write build manifest.
    exit /b 1
)
exit /b 0

:publish_artifacts
set "ARTIFACT_VERSION_DIR=%ARTIFACTS_ROOT%\%SDK_BUILD_VERSION%\%CONFIG%"
set "ARTIFACT_GAME_DIR=%ARTIFACT_VERSION_DIR%\game"
set "ARTIFACT_SYMBOLS_DIR=%ARTIFACT_VERSION_DIR%\symbols_debug"
set "ARTIFACT_ARCHIVE_DIR=%ARTIFACTS_ROOT%\archive"

if exist "%ARTIFACT_VERSION_DIR%" (
    if not exist "%ARTIFACT_ARCHIVE_DIR%" mkdir "%ARTIFACT_ARCHIVE_DIR%"
    call :compress_directory "%ARTIFACT_VERSION_DIR%" "%ARTIFACT_ARCHIVE_DIR%\artifact_%SDK_BUILD_VERSION%_%CONFIG%_%LOGSTAMP%.zip"
    if errorlevel 1 exit /b 1
    rmdir /s /q "%ARTIFACT_VERSION_DIR%" >nul 2>nul
)

mkdir "%ARTIFACT_GAME_DIR%" >nul 2>nul
mkdir "%ARTIFACT_SYMBOLS_DIR%" >nul 2>nul

robocopy "%GAMEDIR%" "%ARTIFACT_GAME_DIR%" /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
if %ERRORLEVEL% GEQ 8 (
    echo Failed to copy game files into artifacts.
    exit /b 1
)

if exist "%SYMBOLSDIR%" (
    robocopy "%SYMBOLSDIR%" "%ARTIFACT_SYMBOLS_DIR%" /MIR /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
    if %ERRORLEVEL% GEQ 8 (
        echo Failed to copy symbol files into artifacts.
        exit /b 1
    )
)

call :write_build_manifest
if errorlevel 1 exit /b 1

copy /y "%BUILD_MANIFEST_GAME%" "%ARTIFACT_GAME_DIR%\.r5sdk_build.json" >nul
if errorlevel 1 (
    echo Failed to copy build manifest into artifact game folder.
    exit /b 1
)

call :compress_directory "%ARTIFACT_GAME_DIR%" "%ARTIFACT_VERSION_DIR%\game.zip"
if errorlevel 1 exit /b 1

if exist "%ARTIFACT_SYMBOLS_DIR%" (
    call :compress_directory "%ARTIFACT_SYMBOLS_DIR%" "%ARTIFACT_VERSION_DIR%\symbols_debug.zip"
    if errorlevel 1 exit /b 1
)

set "LATEST_MANIFEST=%ARTIFACTS_ROOT%\latest.json"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$latest = [ordered]@{ version = [int64]$env:SDK_BUILD_VERSION; source_version = [int64]$env:SDK_BUILD_VERSION; build_config = $env:CONFIG; target = $env:TARGET; artifact_dir = $env:ARTIFACT_VERSION_DIR; game_zip = (Join-Path $env:ARTIFACT_VERSION_DIR 'game.zip'); symbols_debug_zip = (Join-Path $env:ARTIFACT_VERSION_DIR 'symbols_debug.zip'); built_at = (Get-Date).ToString('o') }; $latest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $env:LATEST_MANIFEST -Encoding UTF8"
if errorlevel 1 (
    echo Failed to update artifact latest manifest.
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

:sync_vscripts
if not exist "%VSCRIPT_SRC%" exit /b 0

echo Syncing SDK vscript resources...
if not exist "%VSCRIPT_DEST%" mkdir "%VSCRIPT_DEST%"
xcopy "%VSCRIPT_SRC%\*" "%VSCRIPT_DEST%\" /E /I /Y >nul
if errorlevel 1 (
    echo Failed to sync SDK vscript resources.
    exit /b 1
)
exit /b 0

:sync_mod_defaults
if not exist "%MOD_DEFAULTS_SRC%" exit /b 0

echo Syncing SDK mod default resources...
if not exist "%MOD_DEFAULTS_DEST%" mkdir "%MOD_DEFAULTS_DEST%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference = 'Stop'; $src = $env:MOD_DEFAULTS_SRC; $dst = $env:MOD_DEFAULTS_DEST; $manifest = $env:MOD_DEFAULTS_MANIFEST; $managed = @('R5DisconnectMessages'); if (Test-Path -LiteralPath $manifest) { $managed += @(Get-Content -LiteralPath $manifest | Where-Object { $_ -and [IO.Path]::GetFileName($_) -eq $_ }) }; foreach ($name in ($managed | Select-Object -Unique)) { $target = Join-Path $dst $name; if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force } }; $items = @(Get-ChildItem -LiteralPath $src -Force); foreach ($item in $items) { Copy-Item -LiteralPath $item.FullName -Destination $dst -Recurse -Force }; @($items | Select-Object -ExpandProperty Name) | Set-Content -LiteralPath $manifest -Encoding ASCII"
if errorlevel 1 (
    echo Failed to sync SDK mod default resources.
    exit /b 1
)
exit /b 0

:separate_debug_files
if not exist "%GAMEDIR%" (
    echo Debug file separation skipped: game output directory was not found.
    exit /b 0
)

set "DEBUG_FILE_COUNT=0"
for /r "%GAMEDIR%" %%F in (*.pdb *.ilk *.iobj *.ipdb *.exp *.lib) do (
    set /a DEBUG_FILE_COUNT+=1
)

if "%DEBUG_FILE_COUNT%"=="0" (
    echo No debug files found under "%GAMEDIR%".
    exit /b 0
)

if not exist "%SYMBOLS_ROOT%" mkdir "%SYMBOLS_ROOT%"
if exist "%SYMBOLSDIR%" rmdir /s /q "%SYMBOLSDIR%"
mkdir "%SYMBOLSDIR%"

robocopy "%GAMEDIR%" "%SYMBOLSDIR%" *.pdb *.ilk *.iobj *.ipdb *.exp *.lib /S /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
if %ERRORLEVEL% GEQ 8 (
    echo Failed to copy debug files from game output.
    exit /b 1
)

for /r "%GAMEDIR%" %%F in (*.pdb *.ilk *.iobj *.ipdb *.exp *.lib) do (
    del /q "%%~fF" >nul 2>nul
)

if exist "%SYMBOLS_ROOT%\latest" rmdir /s /q "%SYMBOLS_ROOT%\latest"
robocopy "%SYMBOLSDIR%" "%SYMBOLS_ROOT%\latest" /E /R:2 /W:1 /NFL /NDL /NJH /NJS >nul
if %ERRORLEVEL% GEQ 8 (
    echo Failed to update latest debug files.
    exit /b 1
)

echo Moved %DEBUG_FILE_COUNT% debug file(s) out of game: %SYMBOLSDIR%
exit /b 0

# make_deploy.ps1 - build the portable deploy folder and the desktop shortcut.
# ASCII only (the shell-write guard blocks non-ASCII .ps1 on this machine).
#
#   pwsh -NoProfile -File tools\make_deploy.ps1
#   pwsh -NoProfile -File tools\make_deploy.ps1 -NoShortcut
#
# Produces:
#   <repo>\deploy\  ttsc-server.exe, ttsc-tests.exe, qwen-tts.exe, ggml*.dll,
#                   ttsserver.ini, run.cmd
#   %USERPROFILE%\Desktop\TtsEngine-C.lnk -> deploy\run.cmd
param(
    [string]$BuildDir = "build-cuda",
    # Source config. Empty = <repo>\ttsserver.ini when present, otherwise the
    # config already in deploy\ (so re-deploying keeps the current settings).
    [string]$Ini      = "",
    [switch]$NoShortcut
)

$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
$src  = Join-Path $repo $BuildDir
$dst  = Join-Path $repo "deploy"

if (-not (Test-Path (Join-Path $src "ttsc-server.exe"))) {
    throw "no ttsc-server.exe in $src - build first"
}
if (-not $Ini) {
    $repoIni = Join-Path $repo "ttsserver.ini"
    $dstIni  = Join-Path $dst "ttsserver.ini"
    if (Test-Path $repoIni) { $Ini = $repoIni }
    elseif (Test-Path $dstIni) { $Ini = $dstIni }
}
if (-not $Ini -or -not (Test-Path $Ini)) {
    throw "config not found: pass -Ini <path> (see ttsserver.ini.example)"
}

New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item (Join-Path $src "ttsc-launch.exe") $dst -Force
Copy-Item (Join-Path $src "ttsc-gui.exe")    $dst -Force
Copy-Item (Join-Path $src "ttsc-server.exe") $dst -Force
Copy-Item (Join-Path $src "ttsc-tests.exe")  $dst -Force
Copy-Item (Join-Path $src "qwen-tts.exe")    $dst -Force
Copy-Item (Join-Path $src "*.dll")           $dst -Force
Copy-Item $Ini (Join-Path $dst "ttsserver.ini") -Force

$runner = @'
@echo off
rem TtsEngine-C launcher (ASCII only on purpose).
rem ggml-cuda.dll needs the CUDA runtime on PATH or the exe exits 0xC0000135.
if defined CUDA_PATH set "PATH=%CUDA_PATH%\bin\x64;%CUDA_PATH%\bin;%~dp0;%PATH%"
if not defined CUDA_PATH set "PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0\bin;%~dp0;%PATH%"
cd /d "%~dp0"
title TtsEngine-C  (ttsc-server)

if /i "%~1"=="tests" (
    ttsc-tests.exe
    goto :eof
)
if /i "%~1"=="diag" (
    shift
    ttsc-server.exe -c "%~dp0ttsserver.ini" --stretch "%~1" -o "%~2" --rate %~3 %4 %5 %6
    goto :eof
)

echo ============================================================
echo  TtsEngine-C  -  local TTS service
echo  endpoint : http://127.0.0.1:8081   (see ttsserver.ini)
echo  config   : %~dp0ttsserver.ini
echo  stop     : Ctrl+C  or close this window
echo ============================================================
ttsc-server.exe -c "%~dp0ttsserver.ini"
echo.
echo server exited, press any key to close...
pause >nul
'@
Set-Content -Path (Join-Path $dst "run.cmd") -Value $runner -Encoding ASCII

Write-Host "[deploy] -> $dst"
Get-ChildItem $dst | Select-Object Name, Length | Format-Table -AutoSize

if (-not $NoShortcut) {
    $ws = New-Object -ComObject WScript.Shell
    $lnkPath = Join-Path ([Environment]::GetFolderPath("Desktop")) "TtsEngine-C.lnk"
    $lnk = $ws.CreateShortcut($lnkPath)
    $lnk.TargetPath       = Join-Path $dst "ttsc-launch.exe"
    $lnk.WorkingDirectory = $dst
    $lnk.IconLocation     = (Join-Path $dst "ttsc-gui.exe") + ",0"
    $lnk.Description      = "TtsEngine-C local TTS service (C++/GGML)"
    $lnk.Save()
    Write-Host "[deploy] shortcut -> $lnkPath"
}

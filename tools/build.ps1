# =============================================================================
#  build.ps1 - TtsEngine-C build entry point (Windows / MSVC / optional CUDA)
#
#  Why a script instead of a one-liner:
#    The CUDA toolkit may be installed outside the standard location and may
#    lack the Visual Studio integration component, so the Ninja generator + a
#    direct nvcc is used (pass -CudaRoot for custom locations; the default is
#    $env:CUDA_PATH). MSVC environment variables are imported from the Visual
#    Studio DevShell module.
#
#  Usage:
#    pwsh -File tools\build.ps1                  # service + engine (repo root)
#    pwsh -File tools\build.ps1 -UpstreamOnly    # upstream tree only (old mode)
#    pwsh -File tools\build.ps1 -Cpu             # CPU-only build
#    pwsh -File tools\build.ps1 -Clean           # wipe the build tree first
#
#  The default configures the repository root, which builds ttsc-server,
#  ttsc-tests and the engine library; qwen-tts.exe (used by bench-cli.ps1) is
#  built afterwards as an explicit target because the vendored tools are
#  EXCLUDE_FROM_ALL.
# =============================================================================
[CmdletBinding()]
param(
    [string]$BuildDir = "build",
    [switch]$Cpu,
    [switch]$Clean,
    [switch]$UpstreamOnly,
    [string]$VsPath = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    # Defaults to $env:CUDA_PATH (set by the toolkit installer); override for
    # custom install locations.
    [string]$CudaRoot = $(if ($env:CUDA_PATH) { $env:CUDA_PATH } else { "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.0" }),
    [string]$Arch = "120a-real",
    # Extra -D flags for A/B experiments, e.g. -ExtraCmake "-DGGML_CUDA_FORCE_MMQ=ON"
    [string[]]$ExtraCmake = @()
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$src  = if ($UpstreamOnly) { Join-Path $root "third_party\qwentts.cpp" } else { $root }
$out  = Join-Path $root $BuildDir

# --- import the MSVC environment (cl.exe, INCLUDE, LIB, Ninja on PATH) -------
Import-Module (Join-Path $VsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

# Must come AFTER Enter-VsDevShell: the dev shell sets VSLANG from the installed
# locale, which would win over an earlier assignment. Ninja parses MSVC
# /showIncludes to build its dependency graph and only matches the English
# prefix; a localized (CJK) cl.exe prints it translated, so header edits stop
# triggering rebuilds and the include list floods the log.
$env:VSLANG = "1033"

# --- locate tools -----------------------------------------------------------
$cmake = Join-Path $VsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = (Get-Command cmake.exe).Source }
if (-not (Test-Path $cmake)) { throw "cmake.exe not found" }

# --- configure --------------------------------------------------------------
if ($Clean -and (Test-Path $out)) { Remove-Item -Recurse -Force $out }

$argsList = @(
    "-S", $src,
    "-B", $out,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)

if ($Cpu) {
    Write-Host "[build] backend: CPU"
    $argsList += "-DGGML_CUDA=OFF"
} else {
    if (-not (Test-Path (Join-Path $CudaRoot "bin\nvcc.exe"))) {
        throw "CUDA Toolkit not found at $CudaRoot (pass -CudaRoot or use -Cpu)"
    }
    Write-Host "[build] backend: CUDA  ($CudaRoot, arch $Arch)"
    $argsList += @(
        "-DGGML_CUDA=ON",
        "-DCMAKE_CUDA_COMPILER=$(Join-Path $CudaRoot 'bin\nvcc.exe')",
        "-DCUDAToolkit_ROOT=$CudaRoot",
        "-DCMAKE_CUDA_ARCHITECTURES=$Arch",
        "-DGGML_CUDA_FA_ALL_QUANTS=ON"
    )
}

& $cmake @argsList
if ($LASTEXITCODE -ne 0) { throw "configure failed ($LASTEXITCODE)" }

# Re-apply A/B flags on the *same* build tree so only the affected targets
# recompile (the default configure above does not clear a cached option).
if ($ExtraCmake.Count -gt 0) {
    & $cmake -S $src -B $out @ExtraCmake
    if ($LASTEXITCODE -ne 0) { throw "re-configure failed ($LASTEXITCODE)" }
}

# --- build ------------------------------------------------------------------
$jobs = [Environment]::ProcessorCount
& $cmake --build $out --config Release --parallel $jobs
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }

# The vendored CLI tools are EXCLUDE_FROM_ALL in the root build; the benchmark
# script drives qwen-tts.exe, so build it here rather than in every clean run
# of the service.
if (-not $UpstreamOnly) {
    & $cmake --build $out --config Release --parallel $jobs --target qwen-tts
    if ($LASTEXITCODE -ne 0) { throw "qwen-tts build failed ($LASTEXITCODE)" }
}

Write-Host "[build] done -> $out"

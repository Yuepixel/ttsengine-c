# fetch_upstream.ps1 - check out the pinned qwentts.cpp tree into third_party
#
#   pwsh -NoProfile -File tools\fetch_upstream.ps1
#   pwsh -NoProfile -File tools\fetch_upstream.ps1 -Ref <commit> -Force
#
# The upstream engine is MIT (see THIRD_PARTY_NOTICES.md). It is kept out of
# this repository so its own history and license stay authoritative; this
# script pins the exact revision the release was built and tested against.
param(
    [string]$Ref = "5916bf7",
    [string]$Repo = "https://github.com/ServeurpersoCom/qwentts.cpp.git",
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dst  = Join-Path $root "third_party\qwentts.cpp"

if (Test-Path $dst) {
    if (-not $Force) { throw "$dst already exists (use -Force to re-checkout)" }
    Remove-Item -Recurse -Force $dst
}

New-Item -ItemType Directory -Force -Path (Split-Path $dst -Parent) | Out-Null

# Corporate/legacy proxies often break git; mirror the build docs and go direct.
$env:GIT_TERMINAL_PROMPT = "0"
git -c http.proxy= clone --recurse-submodules $Repo $dst
if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }

git -C $dst checkout $Ref
if ($LASTEXITCODE -ne 0) { throw "git checkout $Ref failed ($LASTEXITCODE)" }

git -C $dst submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "submodule update failed ($LASTEXITCODE)" }

Write-Host "[fetch] $dst @ $Ref"

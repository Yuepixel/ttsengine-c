# =============================================================================
#  bench-cli.ps1 - repeated single-utterance benchmark for qwen-tts.exe
#
#  Runs the same text N times in fresh processes (so the model load cost is
#  visible and measured) and reports the median of the engine's own [Perf]
#  lines. Use it to compare build flags, quantisation levels and models.
#
#  Usage:
#    pwsh -File tools\bench-cli.ps1 -Text "..." -Runs 3
#    pwsh -File tools\bench-cli.ps1 -TextFile .\text.txt -Tag "mmq" -ModelsRoot D:\models
# =============================================================================
[CmdletBinding()]
param(
    [string]$Text = "",
    [string]$TextFile = "",
    # Model folder holding qwen3-tts-gguf/; override with -ModelsRoot or $env:TTSC_MODELS.
    [string]$ModelsRoot = $(if ($env:TTSC_MODELS) { $env:TTSC_MODELS } else { "models" }),
    [string]$Model = "",
    [string]$Codec = "",
    [string]$Lang = "chinese",
    [int]$Runs = 3,
    [string]$Tag = "run",
    [string]$BuildDir = "build-cuda",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path (Join-Path $root $BuildDir) "qwen-tts.exe"
if (-not (Test-Path $exe)) { throw "missing $exe (build first)" }

if (-not $OutDir) { $OutDir = Join-Path $root "out\bench" }
$gguf = Join-Path $ModelsRoot "qwen3-tts-gguf"
if (-not $Model) { $Model = Join-Path $gguf "qwen-talker-1.7b-base-Q8_0.gguf" }
if (-not $Codec) { $Codec = Join-Path $gguf "qwen-tokenizer-12hz-Q8_0.gguf" }

# The engine loads ggml-cuda.dll / cublas64_13.dll from PATH next to the exe.
if ($env:CUDA_PATH) { $env:PATH = "$env:CUDA_PATH\bin\x64;$env:CUDA_PATH\bin;$env:PATH" }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$inFile = Join-Path $OutDir "input.txt"
if ($TextFile) { Copy-Item -Force $TextFile $inFile }
else {
    if (-not $Text) { throw "-Text or -TextFile required" }
    [IO.File]::WriteAllText($inFile, $Text + "`n", [Text.UTF8Encoding]::new($false))
}

$rows = @()
for ($i = 1; $i -le $Runs; ++$i) {
    $wav = Join-Path $OutDir ("out-$Tag-$i.wav")
    $err = Join-Path $OutDir ("run-$Tag-$i.err")
    $sw  = [Diagnostics.Stopwatch]::StartNew()
    $p = Start-Process -FilePath $exe -NoNewWindow -PassThru `
        -ArgumentList @('--model', $Model, '--codec', $Codec, '--lang', $Lang, '-o', $wav) `
        -RedirectStandardInput $inFile -RedirectStandardOutput (Join-Path $OutDir "out.log") `
        -RedirectStandardError $err
    $p.WaitForExit()
    $sw.Stop()

    $txt   = [IO.File]::ReadAllText($err)
    $pick  = {
        param($name)
        $m = [regex]::Match($txt, "\[Perf\]\s+$name\s+([0-9.]+)\s+ms")
        if ($m.Success) { [double]$m.Groups[1].Value } else { $null }
    }
    $rtf = [regex]::Match($txt, "RTF\s+([0-9.]+)")
    $rows += [pscustomobject]@{
        run         = $i
        wall_s      = [math]::Round($sw.Elapsed.TotalSeconds, 2)
        prefill_ms  = & $pick "Prefill"
        ttfa_ms     = & $pick "TTFA"
        talker_ms   = & $pick "TalkerDecode"
        codepred_ms = & $pick "CodePredictor"
        codec_ms    = & $pick "CodecDecode"
        total_ms    = & $pick "Total"
        rtf         = if ($rtf.Success) { [double]$rtf.Groups[1].Value } else { $null }
    }
}

$rows | Format-Table -AutoSize
$med = @{}
foreach ($k in 'prefill_ms','ttfa_ms','talker_ms','codepred_ms','codec_ms','total_ms','rtf') {
    $vals = $rows | ForEach-Object { $_.$k } | Where-Object { $_ -ne $null } | Sort-Object
    if ($vals) { $med[$k] = $vals[[int]($vals.Count / 2)] }
}
"median[{0}] total={1} ms  codec={2} ms  codepred={3} ms  RTF={4}" -f `
    $Tag, $med['total_ms'], $med['codec_ms'], $med['codepred_ms'], $med['rtf']

# Start llama-server for Hoverdock Search (CPU / system RAM, VRAM left free for games).
# Usage:
#   .\start-llama-server.ps1
#   .\start-llama-server.ps1 -ModelPath "D:\models\foo.gguf" -Ngl 0 -Context 2048 -Port 8080
#   .\start-llama-server.ps1 -NoMtp   # disable MTP speculative decoding
#
# Binary: tools/llama-server/llama-server.exe (llama.cpp b11405 Windows CPU release).
# Default model: Qwen3.8-27B-Q4_K_M under the LM Studio models cache. HARD: do not swap models.
#
# Best measured (2026-10-06, -ngl 0, same Q4_K_M):
#   MTP + mmap -c 1024 -t 12 -fa on -b 256 -ub 64 -cram 0 --spec-draft-n-max 2
#   -> gen ~3.31 tok/s (code prompt), peak private ~12.9 GB, peak WS ~19.8 GB (mmap file pages)
#   Non-MTP lean (-lm none -c 2048 -t 8): ~1.9 tok/s, WS/private ~16 GB
# Target 20 tok/s is not reachable on CPU for 27B Q4; MTP is the measured speed path.
# Slots: -np 1 keeps a single KV slot (auto parallel was allocating 4x ctx).

param(
    [string]$ModelPath = "",
    [int]$Ngl = 0,
    [int]$Context = 1024,
    [int]$Port = 8080,
    [string]$HostAddress = "127.0.0.1",
    [int]$Parallel = 1,
    [int]$Threads = 12,
    [int]$ThreadsBatch = 12,
    [int]$Batch = 256,
    [int]$Ubatch = 64,
    [int]$SpecDraftNMax = 2,
    [ValidateSet("mmap","none","mlock")]
    [string]$LoadMode = "mmap",
    [switch]$NoMtp
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$server = Join-Path $here "llama-server.exe"
if (-not (Test-Path $server)) {
    Write-Error "Missing $server - extract llama-b*-bin-win-cpu-x64.zip into this folder."
}

if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $candidates = @(
        "$env:USERPROFILE\.cache\lm-studio\models\lmstudio-community\Qwen3.8-27B-GGUF\Qwen3.8-27B-Q4_K_M.gguf",
        "$env:USERPROFILE\.lmstudio\models\lmstudio-community\Qwen3.8-27B-GGUF\Qwen3.8-27B-Q4_K_M.gguf"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $ModelPath = $c; break }
    }
    if ([string]::IsNullOrWhiteSpace($ModelPath)) {
        Write-Error "Qwen3.8-27B-Q4_K_M.gguf not found under .cache\lm-studio\models\...\Qwen3.8-27B-GGUF. Pass -ModelPath."
    }
}

$specArgs = @()
if (-not $NoMtp) {
    $specArgs = @("--spec-type", "draft-mtp", "--spec-draft-n-max", "$SpecDraftNMax")
}

$extra = @(
    "-fa", "on",
    "-t", "$Threads",
    "-tb", "$ThreadsBatch",
    "-lm", $LoadMode,
    "-b", "$Batch",
    "-ub", "$Ubatch",
    "-cram", "0"
)

$flagNote = "-ngl $Ngl -c $Context -np $Parallel -fa on -t $Threads -tb $ThreadsBatch -lm $LoadMode -b $Batch -ub $Ubatch -cram 0 --host $HostAddress --port $Port --jinja --no-reasoning-preserve"
if ($specArgs.Count -gt 0) {
    $flagNote += " --spec-type draft-mtp --spec-draft-n-max $SpecDraftNMax"
} else {
    $flagNote += " (MTP off; omit -NoMtp to enable)"
}

Write-Host "Model : $ModelPath"
Write-Host "Flags : $flagNote"
Write-Host "API   : http://${HostAddress}:$Port/v1/chat/completions"
Write-Host "Note  : Weights via -lm $LoadMode, n_gpu_layers=$Ngl (VRAM free for games). Dock client sends enable_thinking=false."
Write-Host "Perf  : Best measured gen ~3.31 tok/s with MTP+mmap on this model; 20 tok/s not reachable on CPU for 27B."

& $server -m $ModelPath -ngl $Ngl -c $Context -np $Parallel --host $HostAddress --port $Port --jinja --no-reasoning-preserve @extra @specArgs

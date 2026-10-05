# Start llama-server for Hoverdock Search (CPU / system RAM, VRAM left free).
# Usage:
#   .\start-llama-server.ps1
#   .\start-llama-server.ps1 -ModelPath "D:\models\foo.gguf" -Ngl 0 -Context 4096 -Port 8080
#
# Binary: tools/llama-server/llama-server.exe (llama.cpp b11405 Windows CPU release).
# Default model: Qwen3.8-27B-Q4_K_M under the LM Studio models cache.

param(
    [string]$ModelPath = "",
    [int]$Ngl = 0,
    [int]$Context = 4096,
    [int]$Port = 8080,
    [string]$HostAddress = "127.0.0.1"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$server = Join-Path $here "llama-server.exe"
if (-not (Test-Path $server)) {
    Write-Error "Missing $server — extract llama-b*-bin-win-cpu-x64.zip into this folder."
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

Write-Host "Model : $ModelPath"
Write-Host "Flags : -ngl $Ngl -c $Context --host $HostAddress --port $Port --jinja --no-reasoning-preserve"
Write-Host "API   : http://${HostAddress}:$Port/v1/chat/completions"
Write-Host "Note  : Weights in system RAM (n_gpu_layers=$Ngl). Dock client also sends enable_thinking=false."

& $server -m $ModelPath -ngl $Ngl -c $Context --host $HostAddress --port $Port --jinja --no-reasoning-preserve

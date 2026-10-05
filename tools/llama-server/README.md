# llama-server for Hoverdock Search

Hoverdock Search talks to a local **llama.cpp** `llama-server` over OpenAI-compatible
`/v1/chat/completions` on `http://127.0.0.1:8080` (override with `LlamaServerUrl` in
`dock.ini` or `HOVERDOCK_LLAMA_URL`).

## Binary

Extract a maintained Windows CPU release from
https://github.com/ggml-org/llama.cpp/releases into this folder so that
`llama-server.exe` sits next to this README.

Current machine extract: **b11405** `llama-b11405-bin-win-cpu-x64.zip`.

DLLs/exes are gitignored; re-download if missing:

```powershell
curl.exe -L -o llama-b11405-bin-win-cpu-x64.zip `
  https://github.com/ggml-org/llama.cpp/releases/download/b11405/llama-b11405-bin-win-cpu-x64.zip
Expand-Archive llama-b11405-bin-win-cpu-x64.zip -DestinationPath . -Force
```

## One-click start (VRAM free for games)

```powershell
.\start-llama-server.ps1
# equivalent:
# llama-server.exe -m <gguf> -ngl 0 -c 4096 --host 127.0.0.1 --port 8080 --jinja
```

`-ngl 0` keeps weights in **system RAM**. Default model path is the LM Studio
catalog download for `Qwen3.8-27B-Q4_K_M.gguf`.

## Qwen3.8 notes

Qwen3.8 uses hybrid Gated DeltaNet; recent llama.cpp builds (including b11405)
support it. Dock Search sends `chat_template_kwargs.enable_thinking=false` and
`/no_think` so reasoning stays off for latency.

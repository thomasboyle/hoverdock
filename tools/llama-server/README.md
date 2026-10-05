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
# llama-server.exe -m <gguf> -ngl 0 -c 4096 -np 1 --host 127.0.0.1 --port 8080 --jinja `
#   --no-reasoning-preserve
# optional MTP (more RAM):
#   .\start-llama-server.ps1 -EnableMtp
```

`-ngl 0` keeps weights in **system RAM**. Default model path is the LM Studio
catalog download for `Qwen3.8-27B-Q4_K_M.gguf`.

**`-np 1`** (one server slot) is the default so KV/prompt cache stays on a
single slot and RAM stays lower. Auto parallel was allocating ~4 slots ×
context and made cold slots expensive.

**MTP speculative decoding** is **off by default** (extra draft-context RAM).
Pass `-EnableMtp` to try `--spec-type draft-mtp --spec-draft-n-max 2` on
Qwen3.8 GGUFs that ship nextn heads. Prefer client-side speedups (fast paths,
smaller prompts) before MTP.

Check it is up: `curl.exe http://127.0.0.1:8080/health` -> `{"status":"ok"}`.

## Qwen3.8 notes

Qwen3.8 uses hybrid Gated DeltaNet; recent llama.cpp builds (including b11405)
support it. Dock Search sends `chat_template_kwargs.enable_thinking=false`,
`cache_prompt=true`, and `/no_think` so reasoning stays off and the static
system prompt stays cached. MTP (when enabled) uses the model's own
`blk.*.nextn.*` tensors — GGUFs without those heads should leave MTP off.

## Agent-in-search (1.1.72+)

Typing a *goal* into dock Search (instead of an app name) runs a small local
agent loop against llama-server. Order of resolution for every query:

1. **Exact app name** ("steam", "open steam" when an app is literally named Steam) - instant.
2. **Direct action, no model** - instant, works with the server down:
   - URL / domain: `go to github.com`, `https://news.ycombinator.com`
   - Known user folder: `open downloads folder`, `open documents`, `desktop folder`
     (Downloads, Documents, Desktop, Pictures, Music, Videos, Home)
   - Existing absolute path: `C:\Users\thoma\Downloads`
   - Common web goals: `play lofi on youtube`, `youtube lofi`, `google rtx 5090`,
     `search for rtx 5090 price`
3. **Replay last successful goal** (exact normalized match, process-local) - instant.
4. **Goal?** The query is treated as a goal when it starts with a verb
   (open, launch, run, play, find, search, go, visit, show, watch, listen, ...)
   and has 2+ words, has 4+ words, contains a URL/domain, or mentions
   folder/website. Otherwise it is a **plain search** (model ranking, fuzzy fallback).
5. For goals, a **confident lexical hit** on the remainder skips the model
   (`open chrome`, `launch steam` when only one app clearly matches). Plain
   search also skips the model when ConfidentAppId is unambiguous.
6. Otherwise **agent mode**: up to 3 model rounds, max 3 actions, ~64 output
   tokens per round. Tools (all validated before anything runs):
   - `search_apps {q}` - catalog lookup, result fed back to the model
   - `launch_app {id}` - catalog id only (same launch/focus path as clicking)
   - `open_url {url}` - http/https only
   - `open_path {path}` - existing local folder or document; UNC paths and
     executables/scripts/shortcuts (.exe .bat .ps1 .lnk .msi ...) are refused
   - `done {say}` - short reply shown on the Search status line
   No shell commands, deletes, elevation or file reads. Status lines show the
   current step; a successful action closes Search like a normal launch.
7. **Server down** (or first round fails): falls back to fuzzy search on the
   remainder; no agent replies are shown.

Example goals: `open chrome`, `launch steam`, `open downloads folder`,
`go to github.com`, `play lofi on youtube`, `search google for rtx 5090 price`,
`find something to edit photos`.

Speed: on CPU (`-ngl 0`) the 27B model does ~6–8 tok/s prompt and ~1–2 tok/s
generation, so a model round still takes tens of seconds. Fast paths 1–3 and 5
never touch the model. Ranking prompts send ≤16 compact candidates with
`cache_prompt=true`; HTTP keepalive reuses the WinHTTP session on the worker
thread. Prefer those client speedups over MTP.

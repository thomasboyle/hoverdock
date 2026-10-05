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
`cache_prompt=true`, and a one-line GBNF grammar (falls back to `/no_think` when a
server rejects the grammar) so reasoning stays off and the static system prompt
stays cached. MTP (when enabled) uses the model's own
`blk.*.nextn.*` tensors — GGUFs without those heads should leave MTP off.

## Agent-in-search (1.1.72+, first-time fast paths 1.1.74)

Typing a *goal* into dock Search (instead of an app name) resolves locally
whenever possible and only asks the model as a last resort. Order for every query:

1. **Exact app name** ("steam", "open steam" when an app is literally named Steam) - instant.
2. **Replay last successful goal** (exact normalized match, process-local) - instant.
3. **Model-free plan** (`PlanWithoutModel`, sub-millisecond, works with the server down):
   - **Direct**: URL/domain (`go to github.com`), absolute path (`C:\Windows`),
     explicit site searches - `play lofi on youtube`, `cats on reddit`,
     `search amazon for usb c cable`, `listen to jazz on spotify`,
     `google rtx 5090 price`, `youtube lofi beats`, `search for rtx 5090 price`,
     `look up tom hanks`, `images of red pandas`, `directions to kings cross`.
   - **Folders**: Downloads, Documents, Desktop, Pictures, Music, Videos, Home,
     Screenshots, OneDrive, AppData, LocalAppData, Temp, Program Files, Saved Games,
     Startup, Recent, Fonts, drives (`c drive`, `d:`), and `<name> folder` for an
     existing folder directly under the profile/known folders/fixed-drive roots
     (`open c++ folder` -> `D:\C++`). `in explorer` suffix is ignored.
   - **Confident catalog app** after verb/filler stripping: exact or normalized
     name (`vs code`), alias table (`vscode`, `word`, `cmd`, `calc`,
     `task manager`, `ps`, `obs`, `epic games`...), lexical score with margin and
     full keyword coverage, acronym (`vsc`), unique name prefix, 1-2 edit typos
     (`chorme`, `spotfy`, `dicsord`).
   - **Site home / site search** (after the catalog so installed apps win):
     ~55 sites (`open reddit`, `gmail`, `google drive`, `amazon usb c cable`,
     `wikipedia alan turing`, `open chatgpt`). `go to` / `visit` prefer the site.
   - **Questions** -> Google (`what is ...`, `how to ...`, `weather in ...`).
   - **Strong keyword match** ("search_apps" done locally): one app clearly wins
     and covers the goal words (`open the steam client`).
   - **Compound goals** when every part resolves: `open spotify and discord`,
     `open downloads then play lofi on youtube`, `open firefox and go to github.com`.
4. **Model (single shot)** for genuinely novel goals (`find something to edit photos`).
   One request, one grammar-constrained line (GBNF via llama-server `grammar`):
   `L <id>` launch listed app, `W <query>` Google, `Y <query>` YouTube,
   `U <url>` http(s) URL, `P <folder>` folder/path, `S <words>` app search,
   `N <reason>` nothing. `S` is answered locally and launches immediately when
   one app clearly wins; only otherwise a second, launch-only round runs.
   Up to 5 compact candidates (`c3=Steam; c9=GIMP`), `max_tokens` 40.
   Plain (non-goal) queries use the same system prompt with a launch-only
   grammar (`L cN` / `N`) over <= 8 candidates.
5. **Server down** (or first round fails): fuzzy search on the remainder; no agent replies.

All actions are validated before anything runs: `launch_app` is catalog ids only;
URLs http/https only; paths must exist, no UNC, and executables/scripts/shortcuts
(.exe .bat .ps1 .lnk .msi ...) are refused. No shell commands, deletes,
elevation or file reads.

### Latency (27B Q4_K_M, CPU `-ngl 0`: ~7 tok/s prompt, ~0.77 s per output token)

Token counts measured with the Qwen tokenizer against llama-server b11405:

| Path | 1.1.73 | 1.1.74 |
|---|---|---|
| Agent launch (`L c7` vs JSON launch+done) | ~109 prompt + ~21 out tok -> ~30 s | ~50 prompt + ~4 out -> ~10 s |
| Agent web search (`W q` vs JSON open_url) | ~109 + ~35 tok -> ~43 s | mostly local now (0 s); model: ~50 + ~10 -> ~15 s |
| `S` then launch | 2 rounds | local search_apps, 1 round |
| Plain ranking (8 compact vs 16 JSON) | ~351 + ~7 tok -> ~55 s | ~80 + ~4 tok -> ~15 s |
| Cold system prompt (~132 tok, ~19 s) | paid by first query; agent/ranking prompts evicted each other | shared prompt, primed by `WarmPromptCache` when Search opens |

Ranking and agent share one byte-identical system prompt so the single slot
(`-np 1`) keeps it cached (checked with Qwen3-0.6B on b11405: grammar accepted,
`cache_n=132` on follow-up requests, 4 predicted tokens per launch reply; the hybrid
27B relies on llama-server context checkpoints for the same reuse). The
Search worker logs `Search route=<path> worker_ms=<n>` for every query.
If a server rejects the `grammar` field (HTTP 4xx) the client retries once
without it and parses the same compact protocol (plus legacy JSON tool calls).
Prefer these client speedups over MTP (still opt-in via `-EnableMtp`).

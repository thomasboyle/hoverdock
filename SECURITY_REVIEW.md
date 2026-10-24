# SECURITY_REVIEW.md — Hoverdock ("Liquid Glass Dock")

Branch: `security/hardening`. Audit scope: static review of the repository at the commit
this branch is based on. No runtime exploitation or live network calls beyond cloning.

## 1. Executive summary

Top 3 risks (plain language):

1. **Auto-updater trusted the release feed more than it should.** The updater
   downloaded whatever `.exe` URL a GitHub release JSON pointed at, as long as the
   *host* was `github.com`/`objects.githubusercontent.com`. A compromised or
   attacker-controlled release payload could redirect the download to an asset in a
   *different* repository, which the client would then Authenticode-check and run.
   Fixed by pinning asset URLs to the configured owner/repo path and tightening
   redirect/download host allowlists (F1).
2. **CI was a single point of code execution on users.** Workflows pinned
   third-party actions by floating major tag (`@v6`, `@v7`, `@v3`), so a moved tag
   could silently change what runs on every release. The release workflow also
   interpolated `${{ github.ref_name }}` directly into a PowerShell step, a
   script-injection vector. Fixed by pinning to immutable SHAs and routing the
   value through the environment (F2).
3. **Authenticode verification did not check revocation**, and the signer was not
   pinned to a specific publisher, so any trusted code-signing certificate would
   pass. Revocation checking is now enabled (F4); publisher pinning remains an
   open decision (F5) because it needs the expected certificate subject from the
   signing setup.

Overall posture: **Moderate.** The codebase already shows substantial defensive
work (host allowlists, Authenticode gating, batch-file sanitization, size caps,
TLS 1.2 floor), but the auto-update path and CI pipeline were the two places
where a compromise became code execution on every user. The remaining items are
hardening gaps rather than confirmed exploitable bugs.

## 2. Threat model assumptions

- Local unprivileged attacker or same-user malware; network attacker (MITM,
  malicious/compromised release feed); compromised GitHub account/token/CI
  dependency; malicious or malformed data in parsed files (dock.ini, taskbar
  registry blobs, MFT records, XML layouts).
- App runs per-user, not elevated. Elevated paths exist only via the on-demand
  MFT indexer task, which copies a binary into `%ProgramFiles%\Hoverdock`.

## 3. Findings table

| ID | Title | Severity | Confidence | Status |
|---|---|---|---|---|
| F1 | Updater release-asset URLs not pinned to configured repo; host allowlist suffix too broad | High | High | Patched (7eb7c68) |
| F2 | CI actions pinned by moving tag; script injection in release version step | Medium | High | Patched (2873194) |
| F3 | `OpenLocation` command injection via pin target quotes | Medium | Medium | Patched (df27742) |
| F4 | `VerifyAuthenticode` disabled revocation checks | Medium | High | Patched (7eb7c68) |
| F5 | Authenticode does not pin publisher/signer identity | High | Medium | Needs decision |
| F6 | CI installs NSIS from the network unpinned; broad release permissions | Medium | Medium | Open |
| F7 | Installer `taskkill /F /IM Dock.exe` can kill unrelated processes; window-class spoofing | Low | Medium | Open |
| F8 | Missing build-hardening flags (/guard:cf, /CETCOMPAT, /sdl) | Low | High | Backlog |
| F9 | No checksums/SBOM published with releases | Low | High | Backlog |
| F10 | Memory-safety review of MFT/FileSearch parsing incomplete | Medium | Low | Open |
| F11 | Privacy review of GDI capture and logs not completed | Low | Low | Open |

## 4. Per-finding detail

### F1 — Updater release-asset URLs not pinned to configured repo
- Location: `src/Updater.cpp`, `ParseApiReleaseBody` (allowedHost checks), `HttpGetEx`
  redirect handling, `DownloadFile`.
- Trigger: a release JSON (or a MITM that defeats TLS, or a compromised account) lists
  an asset URL under a different repository on `github.com`. The host-only allowlist
  accepted it.
- Impact: the client downloads and (after Authenticode verification) executes a binary
  chosen by the attacker-controlled feed.
- Fix: require `github.com` asset URLs to start with
  `https://github.com/<owner>/<repo>/releases/download/`; restrict redirect/download
  host suffix matching to `.githubusercontent.com` only (dropping `.github.com`, which
  could match attacker-controlled subdomains in principle).
- Trade-offs: objects.githubusercontent.com URLs are still host-only because their
  paths are opaque CDN paths; mitigated by Authenticode. Alternative was to drop
  objects.githubusercontent.com entirely, but that breaks normal downloads.

### F2 — CI actions pinned by moving tag; script injection
- Location: `.github/workflows/build-dock.yml`, `.github/workflows/release.yml`.
- Trigger: a moved tag (`actions/checkout@v6`) or a maliciously named tag
  (`refs/tags/v1.0;…`) pushed by an actor with write access.
- Impact: altered build/release pipeline → code execution on every user via
  auto-update.
- Fix: pin `actions/checkout`, `actions/upload-artifact`, and
  `softprops/action-gh-release` to immutable commit SHAs; route `github.ref` /
  `github.ref_name` through `env:` instead of direct `${{ }}` interpolation.
- Trade-offs: SHAs must be bumped manually to receive upstream fixes; considered
  Dependabot/Renovate automation as follow-up.

### F3 — `OpenLocation` command injection
- Location: `src/WindowCatalog.cpp`, `OpenLocation`.
- Trigger: a pin whose `target` contains a double quote (a malicious dock.ini or a
  crafted taskbar import) reaches the right-click "locate" action.
- Impact: breakout from the `/select,"…"` argument list passed to `explorer.exe`,
  potentially opening/launching an unintended target.
- Fix: reject targets containing control characters or quotes before building the
  argument string, mirroring the existing `LaunchApp` sanity check.

### F4 — Revocation checks disabled
- Location: `src/Updater.cpp`, `VerifyAuthenticode`.
- Trigger: a revoked code-signing certificate would still be accepted.
- Impact: a revoked signer's binary could be installed by the updater.
- Fix: set `fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN`.

### F5 — No publisher pinning (needs decision)
- Location: `src/Updater.cpp`, `VerifyAuthenticode`.
- The check accepts *any* trusted signer. Pinning to the project's certificate
  subject/issuer would close the "compromised GitHub account signs with their own
  trusted cert" path, but requires the expected subject from the signing PFX, which
  is not available in the repo. Decision needed: provide the expected subject (or a
  hash of the signing cert) at build time.

### F6 — CI installs NSIS from the network; broad permissions
- `choco install nsis` runs during the build with no version pin or hash check.
  The release job holds `contents: write` for the whole job. Recommend scoping
  permissions and pinning the NSIS version.

### F7 — Installer process matching
- `CloseDockBody` uses `FindWindow "LiquidGlassDockWindow"` and a final
  `taskkill /F /IM Dock.exe`, which can terminate unrelated processes named
  `Dock.exe`. Severity low (same machine, same user).

### F8 — Build hardening
- CMakeLists.txt enables `/W4 /WX /permissive-` but not `/guard:cf`,
  `/CETCOMPAT`, or `/sdl`. `/GS`, `/DYNAMICBASE`, `/NXCOMPAT` are MSVC defaults.
  Adding them is a low-risk hardening step (may require `/guard:cf` compatible
  with all linked libs).

### F9 — No checksums/SBOM
- Releases ship only the binaries. Publishing SHA-256 sums would let users and
  auditors verify assets out-of-band.

### F10 — MFT/FileSearch memory safety
- `src/FileSearch.cpp` and `src/FileSearchIndex.cpp` parse untrusted on-disk MFT
  records and registry blobs with `memcpy` and size math. A full audit of integer
  overflow / bounds checks there was not completed within budget. The portable index
  tests (`FileSearchIndexTests`) cover the index layer only.

### F11 — Privacy/GDI capture
- The desktop snapshot used for the glass blur is sampled in-process; no evidence
  of persistence was found in the reviewed code, but a full check of temp files
  and logs was not completed.

## 5. Hardening backlog
- Add `/guard:cf /CETCOMPAT /sdl` to `target_compile_options` (F8).
- Pin NSIS version in CI; add SHA-256 sums to release assets (F6, F9).
- Publisher pin in `VerifyAuthenticode` once the expected cert subject is known (F5).
- Make installer process-termination target the dock's PID/window rather than image
  name (F7).
- Complete memory-safety audit of `FileSearch.cpp` MFT record parsing (F10).

## 6. Coverage

Checked:
- `src/Updater.cpp/.h` in full (network, parsing, download, verification, batch staging).
- `.github/workflows/build-dock.yml`, `release.yml`.
- `installer/hoverdock.nsi`.
- `src/DockConfig.cpp/.h` (INI parser, DPAPI secrets, taskbar import).
- `src/WindowCatalog.cpp` launch/open-location paths.
- `CMakeLists.txt` build flags and dependencies.
- `src/Version.h`, README.

Not checked (and why):
- Full memory-safety audit of `FileSearch.cpp`, `Renderer.cpp`, `DockApp.cpp`
  (too large; prioritized trust boundaries per the prompt). Leads only.
- Runtime behavior of updater/installer (no build of installer attempted; Dock
  itself builds successfully on this host with VS 2026 Insiders / MSVC 19.52).
- Branch protection settings and secret scoping (not visible in repo).
- Privacy of GDI capture beyond code inspection.

Build verification: `cmake -S . -B out-build -G "Visual Studio 18 2026" -A x64`
then `cmake --build out-build --config Release` succeeded (Dock.exe produced) after
applying the patches. Tests were not run.

# AGENTS.md — Captura

Captura is a Windows x64 screenshot and annotation app with a native Win32 overlay, Rust PNG encoding, and offline English OCR.

## Commands
- Build (release): `cargo build --locked --release` (requires Windows x64, Rust MSVC, Visual Studio C++ tools, and Windows SDK)
- Stage with OCR: `.\scripts\package.ps1 -StageOnly` (also requires x64 MSVC redistributable files; copies `ocr/` into `target\release`)
- Run (staged dev build): `.\target\release\Captura.exe`
- Test (all): `cargo test --release --locked`
- Test (single): `cargo test --release --locked repeated_saves_keep_both_complete_images`

## Architecture
- `src/main.rs` — Entry point calling `captura_run()` and exporting C ABI functions (`captura_save`, `captura_ocr`, `captura_free`) for atomic PNG saving and offline Tesseract OCR.
- `native/app.cpp` — Win32 message loop, tray icon, global capture shortcut, and overlay interactions.
- `native/editor.hpp` — Annotation/drawing logic; `native/platform.hpp` bridges to Rust and stores settings in `%APPDATA%\com.gloria.captura\settings.ini`.
- `build.rs` — Compiles the C++17 UI (`native/app.cpp`) and embeds `Captura.ico`, `native/app.manifest`, and `native/dialogs.rc`.
- `scripts/package.ps1` — Verifies OCR downloads and stages Tesseract/Leptonica/MSVC DLLs, `eng.traineddata`, and licenses under `ocr/` beside `Captura.exe`.
- **Version Manifests:** `Cargo.toml`, the `captura` package entry in `Cargo.lock`, and `gloria.toml` (keep versions synced across all listed files).

## Personal Rules (Brain)
- **Git Branching:** Never create new branches or git worktrees. Always work directly on `main` until explicitly told otherwise.
- **Versioning:** Never bump versions during normal work. The user picks the version at release time and `/monjita-github-release` sets it in every version file at once; keep version files in sync with each other otherwise.
- **Commit Format:** Write one plain sentence stating what changed and why; do not use `feat:`/`fix:`/`chore:` conventional-commit prefixes.
- **Commit & Push Policy:** Never run `git commit`, `git tag`, or `git push` unless explicitly asked; never force-push to `main`. When the user says "commit and push", verify the build and tests pass, commit on `main`, and push to `origin main`. Pushing never releases anything.
- **Releases:** Releases happen only through `/monjita-github-release`, which bumps the version, pushes, runs `gh workflow run release.yml`, and watches it until the GitHub Release is live. Never create release tags or releases by hand.
- **Path Privacy:** Always use `C:\Users\you` in documentation, comments, and examples—never hardcode the local machine's real username.
- **Release Workflow:** `.github/workflows/release.yml` must be a copy of `GloriaSetup/templates/release.yml` (shared `check`/`release` jobs from `projectGloria/GloriaSetup/.github/workflows/app-{check,release}.yml@main`, plus this repo's own `build` job). Don't hand-write release workflows or pin `gloria-setup`; see `.brain/github-actions.md`.
- **Window Chrome:** Disable native OS titlebars; build an integrated frameless titlebar with drag region, double-click maximize, and custom min/max/close controls.
- **Desktop Shell Lock:** Lock the app shell to `100vw x 100vh` (`overflow: hidden`) with internal scroll panes and `user-select: none` on UI chrome.
- **Scrollbars:** Ban raw OS scrollbars; use thin (4–6px) themed scrollbars with transparent tracks and zero layout shift.
- **Interactive Controls:** Custom-style all form controls; define explicit `hover`, `active`, `focus-visible`, and `disabled` states with snappy 120–180ms transitions.

## Playbooks (Read Before Task)
- **Building & Packaging:** Read `.brain/building-the-project.md` before packaging, compiling release binaries, or building an installer.
- **GitHub Actions & Releases:** Read `.brain/github-actions.md` before creating/modifying `.github/workflows/*.yml`, setting up release automation, or debugging a failed release.

## Done Criteria
- Run `cargo build --locked --release` and `cargo test --release --locked` on Windows x64 for code changes.
- Verify all Version Manifests (`Cargo.toml`, `Cargo.lock`, `gloria.toml`) agree before release; note that the current `.github/workflows/release.yml` still triggers on a version-changing push to `main`.

<!-- brain-sync: 31e69a675214 -->

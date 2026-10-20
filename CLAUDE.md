# WatermelonDS — notes for Claude

Android Nintendo DS emulator (fork of melonDS Android, package `me.magnum.melonds`). Kotlin app + C++ JNI core.

## Repo / git layout
- This folder is its own git repo, nested inside an unrelated parent repo (`GitNew`, remote `Auctions`). Always run git commands from `WatermelonDS/`, never the parent.
- `origin` = `SolaireRocks/WatermelonDS` (the user's fork). `upstream` = `SapphireRhodonite/WatermelonDS` (default branch `Develop`).
- Work happens on branch `custom-onscreen-controls`. Push there; don't open PRs to upstream unless asked.
- Submodules (recursive): `melonDS-android-lib` (emulator core, tracks `GBARumble_PR`), plus oboe, faad2, enet, zstd under `app/src/main/cpp/`.

## Building
- **There is no local Android SDK and only Java 8 on this machine, so you can't build locally.** Push and let GitHub Actions build it.
- Workflow `.github/workflows/main.yaml` ("melonDS Android Nightly Build") runs on push to any branch and takes about 19 minutes. It builds `:app:assembleGitHubNightlyRelease`, moves the `nightly-release` tag, publishes the APK to the "Nightly Build" GitHub release and uploads it as the `melonDS-android` artifact.
- Monitor with `gh run list -R SolaireRocks/WatermelonDS -L 3` and `gh run watch <id> -R SolaireRocks/WatermelonDS --exit-status`. On failure, use `gh run view <id> --log-failed`.
- CI must use `android-actions/setup-android@v4` (v3 fails on "sdkmanager tools") and needs `permissions: contents: write`.
- Flavors: `gitHub`/`playStore` × `prod`/`nightly` (nightly adds the `.nightly` app id suffix, so it installs alongside the main app).

## Encoding gotchas
- Resource/source files must be valid UTF-8. A stray Windows-1252 byte (0x85 instead of `…`) in `strings.xml` once made a menu label render as a "?" box. When writing non-ASCII from scripts, encode as UTF-8 explicitly.
- `core.autocrlf=true` on this machine: working-copy files may be CRLF or LF, but git normalizes on commit. Byte-level script edits need to match the file's current line endings.

## On-screen controls (custom work on this branch)
Adding a new on-screen layout component usually touches all of these:
1. `domain/model/layout/LayoutComponent.kt`: the enum entry. The enum order is the order of the layout editor's "add component" list. Layouts persist components **by enum name** (`PositionedLayoutComponentDto`), so never rename existing entries.
   - `allowsSharedInputs = true` lets a component coexist with others that map to the same `Input` (individual A/B/X/Y next to the ABXY cluster, turbo, combo, Menu). Otherwise `LayoutEditorManagerView.canAddComponent` hides it from the add list.
2. `utils/InputUtils.kt`: its display name (`getStaticLayoutComponentName`, or generated names for turbo/combo/fast-forward slots).
3. `ui/common/componentbuilders/SingleButtonLayoutComponentViewBuilder.kt`: its drawable. Toggle buttons go in `ToggleableSingleButtonLayoutComponentViewBuilder` and `RuntimeLayoutComponentViewBuilderFactory` instead.
4. `ui/emulator/RuntimeLayoutView.kt`: the touch handler in `updateInputs()`, plus the controller-connected hide list in `updateVisibility()` for DS buttons.
5. `res/drawable/button_*.xml`: 64×64 vector. Utility buttons are a grey (`#808080`) rounded square with a white glyph. Face buttons use `@color/face_button_fill`, and turbo ones add `@color/turbo_button_outline`.
6. `res/values/strings.xml`: the label.

Frontend actions (pause, swap screens, fast-forward, etc.) are `Input` entries with `keyCode = -1`, dispatched in `ui/emulator/input/FrontendInputHandler.kt` and implemented in the `frontendInputHandler` object inside `EmulatorActivity.kt`. `Input.PAUSE` opens the pause menu, and both the Pause and Menu buttons use it. `Input.SWAP_SCREENS` swaps the top and bottom screens.

Existing extras: 8-way D-pad (`DPAD_DIAGONAL`), turbo A/B/X/Y, two-button combos (picked via the "Multi-button…" entry; `BUTTON_COMBO_*` aren't listed individually), and Fast Forward 1–3 with per-button speeds in General settings (native `setFastForwardSpeedOverride`).

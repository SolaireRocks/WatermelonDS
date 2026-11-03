# WatermelonDS
A Nintendo DS and DSi emulator for Android, built on top of [melonDS](https://melonds.kuribo64.net/) and the
[melonDS Android port](https://github.com/rafaelvcaetano/melonDS-android) by rafaelvcaetano.

WatermelonDS is a fork that keeps up with upstream while adding RetroAchievements (including offline play), a Vulkan
renderer, RetroArch shader presets and full external display support, wrapped in its own visual identity.



[<img src="https://raw.githubusercontent.com/Kunzisoft/Github-badge/main/get-it-on-github.png" alt="Get it on GitHub" height="80">](https://github.com/SapphireRhodonite/melonDS-android/releases/latest)

<p align="center">
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/187ea254-877e-4efd-a8dd-93a6023684ad" />
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/01e82877-a138-44a3-bc71-c24d48cabab6" />
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/27868058-ce89-470d-afac-b812dea5d1de" />
</p>


# What WatermelonDS adds
Everything from the upstream Android port, plus:

### RetroAchievements
*  Achievements, leaderboards and Hardcore mode, with a shared card style for the in-game popups
*  Your profile and score are shown on load and in the settings screen
*  **Offline play**: unlocks earned without a connection are stored and submitted later. Two providers are available
   the one built into WatermelonDS, or [RAOfflineProxy](https://github.com/SapphireRhodonite) if you prefer to run it
   yourself. Note that RAOfflineProxy does not support Hardcore mode
*  Achievements can be disabled without logging out, and sessions start even for games that have no achievement set

### Renderers
*  Software, OpenGL, Vulkan and compute renderers, with internal resolution scaling
*  Vulkan gained a *fastpath* presentation profile, optimized texture uploads and dynamic scissor clipping
*  **RetroArch shader presets** on every renderer, through [librashader](https://github.com/SnowflakePowered/librashader)
*  Adrenotools driver support on the `gitHub` flavor, for custom Adreno drivers

### External displays
*  Play on a second screen (TV, dock, devices such as the Odin 2), with the touch screen still usable on the handheld
*  Per-ROM external screen and layout settings, screen rotation, backgrounds, video filtering and aspect ratio control
*  Achievements are shown on the external display while you browse your ROM list

### Library and layouts
*  DSiWare titles in the ROM list, box art on the "continue" shelf, and custom ROM names
*  Layout editor with screen transparency, aspect ratio options and separate centering controls
*  Settings backup and restore (requires a restart to apply), with internal and external layouts backed up separately
*  Alternate input bindings and a hold-to-fast-forward button

The core is kept in sync with melonDS 1.0 and later upstream changes.

# About this fork
This is a fork of [WatermelonDS](https://github.com/SapphireRhodonite/WatermelonDS). Its purpose is to add features
that weren't included in the official release. Everything above still applies. The sections below list what this fork
adds on top of it.

Builds from this fork are published to the
[Nightly Build release](https://github.com/SolaireRocks/WatermelonDS/releases/tag/nightly-release). They use the
`me.magnum.melondualds.nightly` package, so they install alongside the regular app.

## On-screen controls
*  **8-way D-pad**: a round D-pad where the diagonals press two directions at once
*  **Turbo A/B/X/Y**: separate turbo buttons, marked with an amber outline
*  **Multi-button combos**: pick "Multi-button…" in the layout editor, choose two buttons, and get one button that
   presses both together
*  **Fast Forward 1–3**: up to three fast-forward buttons, each with its own speed (set in General settings)
*  **Menu button**: opens the pause menu
*  Individual A/B/X/Y buttons can now be placed alongside the ABXY cluster

## Screenshots and gameplay recording
*  New **Screenshot**, **Record** and **Save clip** buttons. Record turns red while recording. All three can also be
   mapped to controller keys
*  Screenshots can be upscaled up to 8x
*  Recordings save the game's screens and audio as MP4. They capture the game itself, not your phone's screen
*  **Save clip** saves the last 30 seconds of gameplay (the length can be changed), like an instant replay
*  You can choose to capture both screens, only the top screen or only the bottom screen
*  Recordings always play back at normal speed, even if you fast-forwarded while recording. The audio stays at full
   volume even when the game is muted or turned down
*  Settings are in Input settings under "Screenshots and recording": screenshot size, recordings folder, resolution,
   frame rate, clip length, and an option to delete recordings left unfinished by a crash or interruption
*  If you press a capture button before choosing a folder, the app asks you to pick one

## Save states
*  **Rolling quick saves**: keep 2–10 quick saves instead of one. Each new quick save replaces the oldest, quick load
   loads the newest, and all of them are listed in the Load state menu
*  **Autosave slot**: saves automatically every 1–60 minutes of play. Time spent paused doesn't count
*  Both are off by default and live in Save files settings

## Normal-speed music during fast-forward
*  The music keeps playing at normal speed while the game is fast-forwarded, instead of speeding up
*  Optionally, sound effects can still play during fast-forward while the music stays at normal speed
*  Settings are in Audio settings: "Normal-speed music on Fast Forward" and "Allow sound effects during Fast Forward
   music"
*  Works with the two most common DS sound engines and picks the right one automatically:
   *  Nintendo's standard sound format, used by most DS games (e.g. Pokémon Diamond/Pearl, HeartGold/SoulSilver,
      Black/White)
   *  Procyon's sound engine, used by games such as Pokémon Mystery Dungeon: Explorers of Time/Darkness/Sky,
      Professor Layton 2–4 and Inazuma Eleven. Vibrato, pitch sweeps and fine tuning aren't reproduced yet
*  In games that use other sound engines, no music is found, so fast-forward is silent. If sound effects are allowed,
   you hear the usual sped-up game audio instead

# Missing Features
*  Local Multiplayer
*  DSi SD card support
*  Customizable button skins

# Performance
Performance is solid on 64 bit devices with thread rendering and JIT enabled, and should run at full speed on flagship
devices. Performance on older devices, specially 32 bit devices, is very poor due to the lack of JIT support.

# Integration with third-party frontends
It's possible to launch WatermelonDS from third party frontends. For that, you will need to have the ROMs you want to
launch already scanned by WatermelonDS. Then, you can configure your third-party frontend with the following
configuration:
*  Package name: `me.magnum.melondualds` (nightly builds use `me.magnum.melondualds.nightly`)
*  Activity name: `me.magnum.melonds.ui.emulator.EmulatorActivity`
*  Parameters (choose one):
    * Intent data (preferred) - a URI of the NDS ROM (ZIP and 7z files are supported). Ensure [read permission is granted](https://developer.android.com/reference/android/content/Intent#FLAG_GRANT_READ_URI_PERMISSION)
    * `uri` (deprecated) - a string with the [SAF](https://developer.android.com/guide/topics/providers/create-document-provider) URI of the NDS ROM (ZIP and 7z files are supported)
    * `PATH` (deprecated) - a string with the absolute path to the NDS ROM (ZIP and 7z files are supported)


### Info regarding save files
When launching ROMs from third-party frontends, if WatermelonDS hasn't scanned that particular ROM previously, it won't
be able to create the save file next to the ROM file if the option "Save next to ROM file" is enabled in the settings or
the save file directory is not set. Instead, WatermelonDS will create a save file in
`Android/data/me.magnum.melondualds/files/saves`

# Releases

Builds are published [here](https://github.com/SapphireRhodonite/melonDS-android/releases). Release candidates are
marked as pre-releases; they can contain more bugs than usual and you may need to clear your app data to get them to
work properly after an update.

# Building
To build the project you will need the Android SDK, NDK, CMake, a JDK 21 toolchain and a Rust toolchain
(`cargo` and `rustup`), which is used to build librashader for each ABI.

## Build steps:
1.  Clone the project, including submodules with:
    
    `git clone --recurse-submodules https://github.com/SapphireRhodonite/melonDS-android.git`
2.  Install the Android SDK, NDK (`28.0.13004108`) and CMake
3.  Install Rust with [rustup](https://rustup.rs/). If Android Studio does not inherit your shell `PATH`, set the
    `CARGO` and `RUSTUP` environment variables to the executable paths
4.  Build with:
    1.  Unix: `./gradlew :app:assembleGitHubProdDebug`
    2.  Windows: `gradlew.bat :app:assembleGitHubProdDebug`
5.  The generated APK can be found at `app/build/outputs/apk/gitHubProd/debug`

There are two flavor dimensions, so build tasks combine both: `gitHub` or `playStore` (the former enables adrenotools)
and `prod` or `nightly`. For example, `:app:assembleGitHubNightlyDebug` or `:app:assembleGitHubProdRelease`.

If you want to create a release build, you will need to modify your `local.properties` file to include the following fields:  
*  `MELONDS_KEYSTORE=<path_to_your_keystore>`
*  `MELONDS_KEYSTORE_PASSWORD=<keystore_password>`
*  `MELONDS_KEY_ALIAS=<name_of_your_key_alias>`
*  `MELONDS_KEY_PASSWORD=<key_alias_password>`

# Credits
*  [melonDS](https://github.com/melonDS-emu/melonDS) by Arisotura and contributors — the emulator core
*  [melonDS Android](https://github.com/rafaelvcaetano/melonDS-android) by rafaelvcaetano
*  [librashader](https://github.com/SnowflakePowered/librashader) — RetroArch shader preset support
*  [rcheevos](https://github.com/RetroAchievements/rcheevos) — RetroAchievements support

# Rockman Dash 2 Enhanced

Unofficial Windows enhancements for the supported Traditional Chinese PC executable. The player package has one `RockmanDash2-Enhanced.exe` containing all three runtime features, plus a separate `Convert-DiscImage.ps1` utility. Original game files and disc images are not included.

## Four Changes

- Traditional Chinese captions for identified real-time cutscenes, synchronized to the game's audio playback. Ordinary native dialogue boxes are excluded. The imported catalog contains 1,144 cues: 20 opening and 1,124 story cues across 86 audio identities in five banks. This is not a claim of complete dialogue coverage or fully reviewed timing.
- XInput controller support, Xbox button names in the native controller settings page, and right-stick mouse-look. Left stick and D-pad move; the current profile targets player 1 / slot 0. Existing game button assignments are preserved.
- Native resolution selection up to 1920x1080, with a guarded fix to the game's video-memory arithmetic. This is higher-resolution rendering, not an aspect-correct 16:9 camera or HUD patch.
- Separate, source-preserving MODE1/2352-to-2048-byte-sector ISO conversion. The conversion does not insert subtitles or change game files.

## Requirements

- Windows 10/11, a writable game installation, and a compatible disc image supplied by the player. Administrator privileges are not requested by the launcher.
- The original `dash2.exe` with SHA-256 `48baddc9250dc6b99da7ac15b3ae68b0c088489b7351f79ffb990e3384dd0ebc`, plus its original game data and sound archives. Other executable versions are rejected before launch.
- The official [Microsoft Visual C++ x86 runtime](https://aka.ms/vs/17/release/vc_redist.x86.exe), required by Xidi even on 64-bit Windows. Installing system prerequisites may require administrator approval; the launcher does not install them automatically.
- Microsoft JhengHei / Traditional Chinese fonts installed through Windows. Fonts are not bundled.

Python, Node.js, ASR models, exported audio, and development tools are not needed to play. This package does not remove the original disc checks, install the obsolete SafeDisc driver, or require disabling security software. It is unsigned; a successful build is not a security audit.

## Play

1. Extract the player ZIP into a separate folder. Copy `RockmanDash2-Enhanced.exe` and [Convert-DiscImage.ps1](Convert-DiscImage.ps1) beside your original `dash2.exe`. Keep the documentation and licenses with any redistributed package.
2. Convert your raw image once, from PowerShell in that game folder:

   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Convert-DiscImage.ps1 -SourcePath .\gamez88_d2.iso
   ```

   This produces `gamez88_d2.windows.iso`. The original is opened read-only. Repeating the conversion reuses identical output but refuses to overwrite different content. Only complete MODE1/2352 data tracks are supported, not MODE2, audio tracks, or mixed-mode layouts. An already-standard ISO is returned unchanged.
3. Double-click `RockmanDash2-Enhanced.exe`. All three features are enabled together. In the original display menu, select an available resolution such as `1920x1080x32`.
4. Keep the launcher console open and exit normally through the game so its own settings writer runs. Only a disc mount created by this launch is ejected; existing mounts are left mounted. Do not run multiple game sessions simultaneously.

For a differently named standard ISO:

```powershell
.\RockmanDash2-Enhanced.exe --iso "D:\Games\MyDisc.windows.iso"
```

The EXE is a single-file distribution, not an in-place rewrite of the game. It embeds the feature DLLs, an internal worker, captions, controller profile, and licenses. Windows loads those modules after they are extracted into an adjacent `dash2-enhanced-<content-id>` directory. No files are extracted into Windows system directories. ISO conversion stays separate; normal mounting and cleanup are internal.

Existing immutable runtime files must match their embedded hashes. Different content is not overwritten or loaded. Player edits to `xinput/Xidi.ini` in that runtime directory are preserved. Use only settings supported by [Xidi 5](https://github.com/samuelgr/Xidi/wiki), and restart after changes. For example, the existing `[Properties]` section supports `MouseSpeedScalingFactorPercent` (100 is the default). Xidi rejects an entire configuration containing unrecognized sections; do not ignore its configuration warnings.

New payloads receive a different runtime directory. Controller profile edits are not automatically migrated between versions. The game executable, its configuration, saves, and original disc image are not replaced by the package. To uninstall, exit the game and remove this EXE and its `dash2-enhanced-*` runtime directory. Keep your game, saves, settings, and images.

## Diagnose

```powershell
.\RockmanDash2-Enhanced.exe --self-test
.\RockmanDash2-Enhanced.exe --diagnose
.\RockmanDash2-Enhanced.exe --check-only
```

- `--self-test`: verify embedded hashes without extraction or game files.
- `--diagnose`: extract and run subtitle, controller-interface, and native graphics diagnostics without a game or disc.
- `--check-only`: also validate the original executable and mount/read the standard ISO, without starting the game.
- `--extract-only`: prepare the runtime without loading any modules.
- `--game-directory PATH`: select an existing directory instead of the EXE's folder. Choose only one diagnostic mode at a time.

Errors remain in the console; a double-click failure also shows a message box. Runtime logs are local, with no upload or telemetry. Existing feature diagnostics can capture the launched game's own foreground client area and record local paths and process information. Inspect logs before sharing them. Captures and debug artifacts do not belong in the source repository or player package.

## Build and Test

From a Windows PowerShell session in this repository:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Test-DiscConversion.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tests\Test-Package.ps1
```

The build fetches checksum-pinned official Zig 0.14.1 and Xidi 5.0.0 archives. Required SafeDiscShim, MinHook, and nlohmann/json sources are vendored. No original game, media extraction, or proprietary SDK is needed to compile. Diagnostics require the runtime prerequisites and a Windows graphics environment; they are not a headless CI guarantee.

Use `-ZigPath PATH` and `-XidiArchivePath PATH` to reuse a trusted existing compiler and the pinned archive. `-ModulesOnly` builds the internal modules. `-PackageOnly` repackages already-built modules and is intended only when module sources have not changed; use a full build after editing feature code. The player EXE is written to `dist/RockmanDash2-Enhanced.exe`.

After testing and committing the matching sources, run [Package.ps1](Package.ps1). It requires a clean Git worktree, validates the recorded inputs and EXE, audits tracked files, and creates the player ZIP, a corresponding-source ZIP, and SHA256SUMS in `dist`. It refuses to replace an existing version's archives. `-CheckOnly` validates the current build inputs without requiring a Git commit or creating archives.

Line-ending conversion is disabled to preserve imported source and catalog hashes. Build products, game assets, images, saves, logs, personal agent configuration, and model environments are ignored by Git. The packaging script also enforces a positive source-file allowlist.

## Verification Limits

The existing three-feature integration was exercised on the development PC. The repository packaging tests cover embedded/extracted bytes, Unicode and bracket paths, valid controller-setting preservation, Xidi error-log checks, rejection of altered modules and unsupported executables, subtitle clock/eligibility diagnostics, native input interfaces, and 1080p offscreen allocation. ISO tests use synthetic sectors and check payload identity, malformed layouts, existing-output protection, and temporary-file cleanup.

The packaged EXE has not yet completed a fresh full gameplay session or been accepted on a second PC. The latest caption additions have not all been replayed in-game. Physical hotplug, arbitrary controller-slot changes, rumble, Alt-Tab recovery, windowed presentation, long playthroughs, and fully aspect-correct widescreen remain unverified or unimplemented. No new Winlator support is promised.

## Source and Rights

See [LICENSE.md](LICENSE.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The compatibility implementation retains SafeDiscShim's GPL-3.0-or-later terms and additional permission; other components retain their upstream licenses. Always give binary recipients access to the matching corresponding source, including when this repository is private.

Caption text is an unofficial adaptation; underlying game dialogue, assets, and trademarks remain with their respective owners. Provenance fields in the imported subtitle JSON refer to historical local review files that are intentionally not distributed. No original audio, runtime memory images, third-party fansub video, screenshots, game executable, saves, or disc images are included. Distribution of adaptations remains subject to the rights applicable to that content; the software license does not grant rights to the original game.
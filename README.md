# Kovaaks OBS Plugin

An OBS Studio plugin that tracks [KovaaK's FPS Aim Trainer](https://kovaaks.com/) runs in real time and displays kill stats and accuracy directly as a browser source overlay, configured through OBS's native settings dialog.

## Features

- **Live kill stats widget** (HTML browser source): accuracy, TTK, hits/shots, efficiency per target, with theme presets.
- **Run averages**: each stat is compared against your average over the last 10 runs.
- **Event-driven run detection**: watches your KovaaK's stats folder via the Windows file system API and reacts instantly to new runs, instead of polling on a timer. Parses the latest CSV and filters out non-gauntlet (target-switching) scenarios.
- **Only active while KovaaK's is running**: the plugin detects the game process and pauses all file watching and exporting when it's closed.
- **Customizable info panel**: configurable sections, custom fields, layout, and alignment, all editable from OBS's own Properties dialog.
- **Hotkeys** for switching sections and re-showing the latest kill stats.

## Requirements

- Windows 10/11, Visual Studio 2022 (MSVC, C++20)
- [CMake](https://cmake.org/) >= 3.28
- A local build of [OBS Studio](https://github.com/obsproject/obs-studio) (source + compiled `obs.lib` / `obs-frontend-api.lib`)
- [nlohmann/json](https://github.com/nlohmann/json) is fetched automatically via CMake `FetchContent`, no manual setup needed.

## Building

1. Clone this repo.
2. Point CMake at your local OBS Studio SDK build:
   ```bash
   cmake -B build -DOBS_ROOT="D:/path/to/obs-studio" -DOBS_INSTALL_DIR="C:/Program Files/obs-studio"
   cmake --build build --config Release
   ```
3. `cmake --install build --config Release` copies the plugin into your OBS Studio's `obs-plugins/64bit` folder.

## Setup in OBS

1. Add a **Kovaaks Plugin Settings** source to any scene (this is the plugin's config source, it produces no video itself).
2. Point it at your KovaaK's save/stats folder and an export folder.
3. Add a **Browser Source** pointing at `kovaaks_widget.html` (kill stats) and/or `kovaaks_killstats.html` from your export folder.
4. Configure sections, custom info and theming from the source's Properties dialog in OBS.
5. Bind hotkeys under Settings → Hotkeys (`Kovaaks: ...`) as needed.

## Fonts

The "Shikuretto" (Huntrix) and "Sukajan" theme presets use custom fonts (`huntrix.ttf`, `sukajan.otf`) that aren't included in this repo. Download them yourself from [FontSpace](https://www.fontspace.com/category/anime) and drop them in the plugin's folder if you want to use those themes, otherwise pick a different preset.

## License

MIT, see [LICENSE](LICENSE).

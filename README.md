# Kovaaks OBS Plugin

An OBS Studio plugin that tracks [KovaaK's FPS Aim Trainer](https://kovaaks.com/) runs in real time and displays kill stats, accuracy, and evolution graphs directly as a browser source overlay, plus a native ImGui settings/debug panel.

## Features

- **Live kill stats widget** (HTML browser source) — accuracy, TTK, hits/shots, efficiency per target, with theme presets (Shikuretto / Sukajan, light & dark).
- **Evolution graph** — per-bot performance curves over the last 10 runs, viewable in an ImGui overlay.
- **Automatic run detection** — watches your KovaaK's stats folder and parses the latest CSV, filtering out non-gauntlet (target-switching) scenarios.
- **Customizable info panel** — configurable sections, custom fields, layout, and alignment.
- **Hotkeys** for switching sections, showing the latest kill stats, and opening the graph/evolution overlays.
- Rendered with [Dear ImGui](https://github.com/ocornut/imgui) over DirectX 11, hooked into OBS's render loop.

## Requirements

- Windows 10/11, Visual Studio 2022 (MSVC, C++20)
- [CMake](https://cmake.org/) ≥ 3.28
- A local build of [OBS Studio](https://github.com/obsproject/obs-studio) (source + compiled `obs.lib` / `obs-frontend-api.lib`)
- Dependencies ([nlohmann/json](https://github.com/nlohmann/json), [Dear ImGui](https://github.com/ocornut/imgui)) are fetched automatically via CMake `FetchContent` — no manual setup needed.

## Building

1. Clone this repo.
2. Point CMake at your local OBS Studio SDK build:
   ```bash
   cmake -B build -DOBS_ROOT="D:/path/to/obs-studio" -DOBS_INSTALL_DIR="C:/Program Files/obs-studio"
   cmake --build build --config Release
   ```
3. `cmake --install build --config Release` copies the plugin into your OBS Studio's `obs-plugins/64bit` folder.

## Setup in OBS

1. Add a **Kovaaks Plugin Settings** source to any scene (this is the plugin's config source; it produces no video itself).
2. Point it at your KovaaK's save/stats folder and an export folder.
3. Add a **Browser Source** pointing at `kovaaks_widget.html` (kill stats) and/or `kovaaks_killstats.html` from your export folder.
4. Bind hotkeys under Settings → Hotkeys (`Kovaaks: ...`) as needed.

## Fonts

This project references two custom font themes ("Shikuretto"/Huntrix and "Sukajan") that are **not included** in this repo due to unclear licensing. To use those themes, supply your own `huntrix.ttf` and `sukajan.otf` files in the plugin's folder — or pick a different theme preset that doesn't require them.

## License

MIT — see [LICENSE](LICENSE).
